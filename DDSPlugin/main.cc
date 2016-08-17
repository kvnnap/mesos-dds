// System includes
#include <cstdlib>
#include <unistd.h>
#include <thread>
#include <fstream>
#include <stdexcept>
#include <chrono>

#include <string>
#include <sstream>

#include <memory>

// Boost includes
#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/filesystem.hpp>

// CppRestSDK (Casablanca)
#include <cpprest/http_client.h>
#include <cpprest/json.h>

//#include <boost/property_tree/json_parser.hpp>
//#include <boost/signals2/signal.hpp>
// Must keep this order when including these header files
#include "dds_intercom.h"
#include "Utils.h"
#include "Structures.h"
#include "Constants.h"

using namespace std;
using namespace dds::intercom_api;
using namespace DDSMesos::Common;

// Use unnamed namespaces in C++ instead
// of static specifier as used in C
namespace {
    const char* const defaultMaster = "localhost:5050";
    const char* const defaultRestHost = "localhost:80";
    const char* const defaultDockerAgentImage = "";
    const char* const defaultTempDirInContainer = "DDSEnvironment";
    const int defaultCpusPerTask = 1;
    const int defaultMemSizePerTask = 1024;
}

int main(int argc, char **argv) {

    // Setup Logging
    Utils::setupLogging("mesos-dds.log");

    // Proceed
    BOOST_LOG_TRIVIAL(trace)
        << "Welcome to dds-submit-mesos" << endl
        << "Argument Count: " << argc << endl;
    for (int i = 0; i < argc; ++i) {
        BOOST_LOG_TRIVIAL(trace) << i << ") " << argv[i] << endl;
    }

    CRMSPluginProtocol protocol("mesos");

    try {

        protocol.onSubmit([&protocol](const SSubmit &submit) {

            std::atomic_bool error ( false );

            // Implement submit related functionality here.
            // After submit has completed call stop() function.

            BOOST_LOG_TRIVIAL(trace)
                 << "DDS-Intercom onSubmit..: " << endl
                 << "\tm_nInstances: " << submit.m_nInstances << endl
                 << "\tm_cfgFilePath: " << submit.m_cfgFilePath << endl
                 << "\tm_id: " << submit.m_id << endl
                 << "\tm_wrkPackagePath: " << submit.m_wrkPackagePath << endl;

            // Inform Mesos to deploy n agents
            DDSSubmitInfo ddsSubmitInfo;
            ddsSubmitInfo.m_cfgFilePath = submit.m_cfgFilePath;
            ddsSubmitInfo.m_id = submit.m_id;
            ddsSubmitInfo.m_nInstances = submit.m_nInstances;
            ddsSubmitInfo.m_wrkPackagePath = submit.m_wrkPackagePath;

            //
            boost::filesystem::path ddsWorkerPackagePath(ddsSubmitInfo.m_wrkPackagePath);

            // Parse config file
            const size_t numLines = 7;
            string conf[numLines];
            if (ddsSubmitInfo.m_cfgFilePath.length() > 0) {
                ifstream ifs(ddsSubmitInfo.m_cfgFilePath);
                for (size_t i = 0; i < numLines && getline(ifs, conf[i]); ++i) {}
            }

            string master = conf[0].length() ? conf[0] : defaultMaster;
            uint32_t numAgents = static_cast<uint32_t >(stoi(conf[1].length() ? conf[1] : string("1")));
            string dockerAgentImage = conf[2].length() ? conf[2] : defaultDockerAgentImage;
            string tempDirInContainer = conf[3].length() ? conf[3] : defaultTempDirInContainer;
            string cpusPerTask (conf[4].length() ? conf[4] : to_string(defaultCpusPerTask));
            string memSizePerTask (conf[5].length() ? conf[5] : to_string(defaultMemSizePerTask));
            string restHost = conf[6].length() ? conf[6] : defaultRestHost;

            BOOST_LOG_TRIVIAL(trace)
                << "Using these values:" << endl
                << "\tmaster: " << master << endl
                << "\tnumAgents: " << numAgents << endl
                << "\tdockerAgentImage: " << dockerAgentImage << endl
                << "\tempDirInContainer: " << tempDirInContainer << endl
                << "\tcpusPerTask: " << cpusPerTask << endl
                << "\tmemSizePerTask: " << memSizePerTask << endl
                << "\trestHost: " << restHost << endl;

            // Send Information through using REST endpoint
            {
                using namespace web;
                using namespace web::http;
                using namespace web::http::client;

                http_client client (string("http://") + restHost + "/" );

                json::value ddsConfInf;

                // Json
                {
                    using namespace DDSMesos::Common::Constants::DDSConfInfo;

                    json::value resources;
                    resources[NumAgents] = json::value::number(numAgents);
                    resources[CpusPerTask] = json::value::string(cpusPerTask);
                    resources[MemorySizePerTask] = json::value::string(memSizePerTask);

                    json::value dockerContainer;
                    dockerContainer[ImageName] = json::value::string(dockerAgentImage);
                    dockerContainer[TemporaryDirectoryName] = json::value::string(tempDirInContainer);

                    ddsConfInf[DDSSubmissionId] = json::value::string(submit.m_id);
                    ddsConfInf[Resources] = resources;
                    ddsConfInf[Docker] = dockerContainer;
                    ddsConfInf[WorkerPackageName] = json::value::string(ddsWorkerPackagePath.filename().string());
                    ddsConfInf[WorkerPackageData] = json::value::string(Utils::encode64(Utils::readFromFile(ddsSubmitInfo.m_wrkPackagePath)));
                }

                // Make request
                std::atomic<uint64_t> submissionId ( 0 );
                client.request(methods::POST, uri_builder("/dds-submit").to_string(), ddsConfInf).then([&error, &submissionId](http_response response) -> void {
                    if (response.status_code() == status_codes::OK) {
                        response.extract_json().then([&error, &submissionId](pplx::task<json::value> responseValue) -> void {
                            try {
                                using namespace DDSMesos::Common::Constants::DDSConfInfoResponse;
                                submissionId = responseValue.get().at(Id).as_number().to_uint64();
                                BOOST_LOG_TRIVIAL(trace) << "OK. REST Server Submison Id: " << submissionId << endl;
                            } catch (const exception& ex) {
                                BOOST_LOG_TRIVIAL(error) << "Malformed response from Server" << endl;
                                error = true;
                            }
                        }).wait();
                    } else {
                        BOOST_LOG_TRIVIAL(error) << "Request Failed - Did not submit" << endl;
                        error = true;
                    }
                }).wait();

                //
                bool ready = false;
                uri_builder builder ("/status");
                builder.append_query("id", to_string(submissionId));
                chrono::system_clock::time_point t1 = chrono::system_clock::now();
                while (!error && !ready) {
                    client.request(methods::GET, builder.to_string()).then([&error, &ready](http_response response) -> void {
                        if (response.status_code() == status_codes::OK) {
                            response.extract_json().then([&error, &ready](pplx::task<json::value> responseValue) -> void {
                                try {
                                    using namespace DDSMesos::Common::Constants::Status;
                                    uint64_t pendingAgents = responseValue.get().at(Status).as_object().at(PendingTasks).as_number().to_uint64();
                                    ready = pendingAgents == 0;
                                    BOOST_LOG_TRIVIAL(trace) << "Pending Agents: " << pendingAgents << endl;
                                } catch (const exception& ex) {
                                    BOOST_LOG_TRIVIAL(error) << "Malformed Status response from Server" << endl;
                                    error = true;
                                }
                            }).wait();
                        } else {
                            BOOST_LOG_TRIVIAL(error) << "Status of Request Failed" << endl;
                            error = true;
                        }
                    }).wait();
                    if (!ready) {
                        this_thread::sleep_for(chrono::milliseconds(500));
                    }
                }
                chrono::system_clock::time_point t2 = chrono::system_clock::now();
                chrono::duration<float> fSec = t2 - t1;
                if (ready) {
                    BOOST_LOG_TRIVIAL(trace) << "Ready! Took: " << fSec.count() << " seconds" << endl;
                }

            }

            // Check for any kind of errors
            if (error) {
                throw runtime_error("Some error occurred, check log!");
            }

            // Call to stop waiting
            protocol.stop();
        });

        protocol.onMessage([](const SMessage &_message) {
            // Message from commander received.
            // Implement related functionality here.
            BOOST_LOG_TRIVIAL(trace) << "DDS-Intercom onMessage: " << _message.m_msg << endl;
        });

        // Let DDS commander know that we are online and wait for notifications from commander
        protocol.start();

    } catch (const exception &e) {
        BOOST_LOG_TRIVIAL(error) << "Mesos DDS-Intercom Exception: " << e.what() << endl;
        // Report error to DDS commander
        protocol.sendMessage(dds::intercom_api::EMsgSeverity::error, e.what());
    }

    BOOST_LOG_TRIVIAL(trace) << "Ready - Exiting Mesos DDS" << endl;

    return 0;
}