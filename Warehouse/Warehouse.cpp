/**
* If not stated otherwise in this file or this component's LICENSE
* file the following copyright and licenses apply:
*
* Copyright 2019 RDK Management
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
**/

#include "Warehouse.h"

#include <algorithm>
#include <fstream>

#include <regex.h>

#if defined(USE_IARMBUS) || defined(USE_IARM_BUS)
#include "libIBus.h"
#include "sysMgr.h"
#include "pwrMgr.h"
#endif

#include "utils.h"

#include "frontpanel.h"

#include "rfcapi.h"

#define WAREHOUSE_RFC_CALLERID                  "Warehouse"
#define WAREHOUSE_HOSTCLIENT_NAME1_RFC_PARAM    "Device.DeviceInfo.X_RDKCENTRAL-COM_RFC.CommonProperties.WarehouseHost.CName1"
#define WAREHOUSE_HOSTCLIENT_NAME2_RFC_PARAM    "Device.DeviceInfo.X_RDKCENTRAL-COM_RFC.CommonProperties.WarehouseHost.CName2"
#define WAREHOUSE_HOSTCLIENT_TAIL_RFC_PARAM     "Device.DeviceInfo.X_RDKCENTRAL-COM_RFC.CommonProperties.WarehouseHost.CNameTail"

#define WAREHOUSE_METHOD_RESET_DEVICE "resetDevice"
#define WAREHOUSE_METHOD_GET_DEVICE_INFO "getDeviceInfo"
#define WAREHOUSE_METHOD_SET_FRONT_PANEL_STATE "setFrontPanelState"
#define WAREHOUSE_METHOD_INTERNAL_RESET "internalReset"
#define WAREHOUSE_METHOD_LIGHT_RESET "lightReset"
#define WAREHOUSE_METHOD_IS_CLEAN "isClean"

#define WAREHOUSE_EVT_DEVICE_INFO_RETRIEVED "deviceInfoRetrieved"
#define WAREHOUSE_EVT_RESET_DONE "resetDone"

#define HOSTS_FILE "/etc/warehouseHosts.conf"
#define DEFAULT_CNAME_TAIL ".warehouse.ccp.xcal.tv"

#define PARAM_SUCCESS "success"
#define PARAM_ERROR "error"

#define DEVICE_INFO_SCRIPT "sh /lib/rdk/getDeviceDetails.sh read"
#define VERSION_FILE_NAME "/version.txt"
#define CUSTOM_DATA_FILE "/lib/rdk/cust-data.conf"

#define LIGHT_RESET_SCRIPT "rm -rf /opt/netflix/* SD_CARD_MOUNT_PATH/netflix/* XDG_DATA_HOME/* XDG_CACHE_HOME/* XDG_CACHE_HOME/../.sparkStorage/ /opt/QT/home/data/* /opt/hn_service_settings.conf /opt/apps/common/proxies.conf /opt/lib/bluetooth"
#define INTERNAL_RESET_SCRIPT "rm -rf /opt/drm /opt/www/whitebox /opt/www/authService && /rebootNow.sh -s WarehouseService &"

#define FRONT_PANEL_NONE -1
#define FRONT_PANEL_INPROGRESS 1
#define FRONT_PANEL_FAILED 3
#define FRONT_PANEL_INTERVAL 5000


namespace WPEFramework
{
    namespace Plugin
    {
        SERVICE_REGISTRATION(Warehouse, 1, 0);

        Warehouse* Warehouse::_instance = nullptr;

        Warehouse::Warehouse()
        : AbstractPlugin()
#ifdef HAS_FRONT_PANEL
        , m_ledTimer(64 * 1024, "LedTimer")
        , m_ledInfo(this)
#endif
        {
            LOGINFO();
            Warehouse::_instance = this;

            registerMethod(WAREHOUSE_METHOD_RESET_DEVICE, &Warehouse::resetDeviceWrapper, this);
            registerMethod(WAREHOUSE_METHOD_GET_DEVICE_INFO, &Warehouse::getDeviceInfoWrapper, this);
            registerMethod(WAREHOUSE_METHOD_SET_FRONT_PANEL_STATE, &Warehouse::setFrontPanelStateWrapper, this);
            registerMethod(WAREHOUSE_METHOD_INTERNAL_RESET, &Warehouse::internalResetWrapper, this);
            registerMethod(WAREHOUSE_METHOD_LIGHT_RESET, &Warehouse::lightResetWrapper, this);
            registerMethod(WAREHOUSE_METHOD_IS_CLEAN, &Warehouse::isCleanWrapper, this);
        }

        Warehouse::~Warehouse()
        {
            LOGINFO();
            Warehouse::_instance = nullptr;

            if (m_resetThread.joinable())
                m_resetThread.join();
        }

        /**
         * @brief This function is used to write the client name in the host file if
         * the string list is empty by default ".warehouse.ccp.xcal.tv" will write in
         * to the host file.
         *
         * @return Attribute of string value list.
         */
        std::vector<std::string> Warehouse::getAllowedCNameTails()
        {
            std::vector<std::string> cnameTails;
            RFC_ParamData_t param;

            memset(&param, 0, sizeof(param));
            WDMP_STATUS wdmpStatus = getRFCParameter(const_cast<char*>(WAREHOUSE_RFC_CALLERID), WAREHOUSE_HOSTCLIENT_NAME1_RFC_PARAM, &param);
            if ( WDMP_SUCCESS == wdmpStatus )
            {
                cnameTails.push_back(std::string(param.value));
                LOGINFO ("getRFCParameter for %s is %s\n", WAREHOUSE_HOSTCLIENT_NAME1_RFC_PARAM, param.value);
            }
            else
                LOGERR ("getRFCParameter for %s Failed : %s\n", WAREHOUSE_HOSTCLIENT_NAME1_RFC_PARAM, getRFCErrorString(wdmpStatus));

            memset(&param, 0, sizeof(param));
            wdmpStatus = getRFCParameter(const_cast<char*>(WAREHOUSE_RFC_CALLERID), WAREHOUSE_HOSTCLIENT_NAME2_RFC_PARAM, &param);
            if ( WDMP_SUCCESS == wdmpStatus )
            {
                cnameTails.push_back(std::string(param.value));
                LOGINFO ("getRFCParameter for %s is %s\n", WAREHOUSE_HOSTCLIENT_NAME2_RFC_PARAM, param.value);
            }
            else
                LOGERR ("getRFCParameter for %s Failed : %s\n", WAREHOUSE_HOSTCLIENT_NAME2_RFC_PARAM, getRFCErrorString(wdmpStatus));

            if (cnameTails.size() == 0)
            {
                memset(&param, 0, sizeof(param));
                wdmpStatus = getRFCParameter(const_cast<char*>(WAREHOUSE_RFC_CALLERID), WAREHOUSE_HOSTCLIENT_TAIL_RFC_PARAM, &param);
                if ( WDMP_SUCCESS == wdmpStatus )
                {
                    cnameTails.push_back(std::string(param.value));
                    LOGINFO ("getRFCParameter for %s is %s\n", WAREHOUSE_HOSTCLIENT_TAIL_RFC_PARAM, param.value);
                }
                else
                    LOGERR ("getRFCParameter for %s Failed : %s\n", WAREHOUSE_HOSTCLIENT_TAIL_RFC_PARAM, getRFCErrorString(wdmpStatus));
            }
            return cnameTails;
        }

        /**
         * @brief Creates a new task for resetting the device.
         * An event will be dispatched after reset is done by notifying success code.
         * @ingroup SERVMGR_WAREHOUSE_API
         */
        void Warehouse::resetDevice()
        {
            resetDevice(false);
        }

#if defined(USE_IARMBUS) || defined(USE_IARM_BUS)
        static void WareHouseResetIARM(Warehouse *wh, bool suppressReboot)
        {
            IARM_Bus_PWRMgr_WareHouseReset_Param_t whParam;
            whParam.suppressReboot = suppressReboot;
            IARM_Result_t err = IARM_Bus_Call(IARM_BUS_PWRMGR_NAME, IARM_BUS_PWRMGR_API_WareHouseReset, &whParam, sizeof(whParam));

            bool ok = err == IARM_RESULT_SUCCESS;
            JsonObject params;

            params[PARAM_SUCCESS] = ok;

            if (!ok)
                params[PARAM_ERROR] = "Reset failed";

            string json;
            params.ToString(json);
            LOGINFO("Notify %s %s\n", WAREHOUSE_EVT_RESET_DONE, json.c_str());
            wh->Notify(WAREHOUSE_EVT_RESET_DONE, params);
        }

        static bool RunScriptIARM(const std::string& script, std::string& error)
        {
            IARM_Bus_SYSMgr_RunScript_t runScriptParam;
            runScriptParam.return_value = -1;
            size_t len = sizeof(runScriptParam.script_path)/sizeof(char);
            if(script.length() > (len - 1))
            {
                std::stringstream errorss;
                errorss << "Length of script greater than allowed limit of " << len << ".";
                error = errorss.str();

                LOGWARN("%s", error.c_str());
                return false;
            }

            strcpy(runScriptParam.script_path, script.c_str());
            IARM_Bus_Call(IARM_BUS_SYSMGR_NAME, IARM_BUS_SYSMGR_API_RunScript, &runScriptParam, sizeof(runScriptParam));
            bool ok = runScriptParam.return_value == 0;

            std::stringstream message;
            message << "script returned: " << runScriptParam.return_value;

            LOGINFO("%s", message.str().c_str());
            if (!ok)
                error = message.str();
            return ok;
        }
#endif

        void Warehouse::resetDevice(bool suppressReboot)
        {
#if defined(USE_IARMBUS) || defined(USE_IARM_BUS)

            LOGWARN("Received request to terminate CoPilot");

            if (m_resetThread.joinable())
                m_resetThread.join();

            m_resetThread = std::thread(WareHouseResetIARM, this, suppressReboot);
#else

            JsonObject params;

            bool ok = false;
            params[PARAM_SUCCESS] = ok;

            if (!ok)
                params[PARAM_ERROR] = "No IARMBUS";

            sendNotify(WAREHOUSE_EVT_RESET_DONE, params);
#endif
        }

        /**
         * @brief Returns a hash of properties for STB Device Information.
         * This includes values for the following keys (uses /lib/rdk/getDeviceDetails.sh):
         * Software version, IP and MAC of ecm, estb, moca etc.
         * @ingroup SERVMGR_WAREHOUSE_API
         */
        void Warehouse::getDeviceInfo(JsonObject &params)
        {
            char buf[256];

            FILE *p = popen(DEVICE_INFO_SCRIPT, "r");

            if(!p)
            {
                LOGWARN("failed to run %s:%s", DEVICE_INFO_SCRIPT, strerror(errno));
                return;
            }

            std::string res;
            while(fgets(buf, sizeof(buf), p) != NULL)
                res += buf;

            int errCode = pclose(p);

            if (0 != errCode)
            {
                params[PARAM_SUCCESS] = false;
                params[PARAM_ERROR] = (const char *)strerror(errno);
            }

            LOGINFO("'%s' returned: %s", DEVICE_INFO_SCRIPT, res.c_str());

            std::stringstream ss(res);
            std::string line;
            while(std::getline(ss, line))
            {
                size_t eq = line.find_first_of("=");

                if (std::string::npos != eq)
                {
                    std::string key = line.substr(0, eq);
                    std::string value = line.substr(eq + 1);

                    params[key.c_str()] = value;

                    // some tweaks for backward compatibility
                    if (key == "imageVersion")
                    {
                        params["version"] = value; 
                        params["software_version"] = value;
                    }
                    else if (key == "cableCardVersion")
                        params["cable_card_firmware_version"] = value;
                }
            }
        }

#ifdef HAS_FRONT_PANEL
        static bool SetFrontPanelLights(int state, int iteration)
        {
            bool ledData = false;
            bool ledRecord = false;
            int ledBrightness = 100;
            if (state == FRONT_PANEL_INPROGRESS)
            {
                // i    0   1   2   3   4   5   6   7   8
                // DA   1   1   1   1   0   0   0   0   1
                // RE   0   0   0   0   1   1   1   1   0
                // BR   100 75  50  25  0   25  50  75  100
                ledData = (iteration / 4) % 2 == 0;
                ledRecord = !ledData;
                ledBrightness = abs(100 - 25 * (iteration % 8));
            }
            else if (state == FRONT_PANEL_FAILED)
            {
                // i    0   1   2
                // DA   1   0   1
                // RE   0   1   0
                // BR   100 100 100
                ledData = iteration % 2 == 0;
                ledRecord = !ledData;
                ledBrightness = 100;
            }
            LOGINFO("SetFrontPanelLights set Brightness=%d (LEDs: Data=%d Record=%d)",
                       ledBrightness, ledData?1:0, ledRecord?1:0);
            CFrontPanel* helper = CFrontPanel::instance();
            bool didSet = false;
            if (ledData)
                didSet |= helper->powerOnLed(FRONT_PANEL_INDICATOR_MESSAGE);
            else
                didSet |= helper->powerOffLed(FRONT_PANEL_INDICATOR_MESSAGE);
            if (ledRecord)
                didSet |= helper->powerOnLed(FRONT_PANEL_INDICATOR_RECORD);
            else
                didSet |= helper->powerOffLed(FRONT_PANEL_INDICATOR_RECORD);
            didSet |= helper->setBrightness(ledBrightness);
            return didSet;
        }
#endif

        /**
         * @brief set the state of the front panel LEDs to indicate the download state of the STB
         * software image. Possible state values: NONE, DOWNLOAD IN PROGRESS, DOWNLOAD FAILED.
         *
         * @param[in] state Integer value of front panel state to set.
         *
         * @return Returns true when all LED states set successfully, else returns false.
         * @ingroup SERVMGR_WAREHOUSE_API
         */
        void Warehouse::setFrontPanelState(int state, JsonObject& response)
        {
#ifdef HAS_FRONT_PANEL
            if (state != FRONT_PANEL_NONE &&
                state != FRONT_PANEL_INPROGRESS &&
                state != FRONT_PANEL_FAILED)
            {
                LOGERR("FrontPanelState incorrect state %d", state);
                response[PARAM_SUCCESS] = false;
                response[PARAM_ERROR] = "incorrect state";
            }
            else
            {
                m_ledTimer.Revoke(m_ledInfo);
                bool didSet = SetFrontPanelLights(state, 0);
                LOGINFO("FrontPanelState %s to %d", didSet ? "set" : "not set", state);
                response[PARAM_SUCCESS] = didSet;
                if (!didSet)
                {
                    response[PARAM_ERROR] = "front panel not set";
                }
                else if (state != FRONT_PANEL_NONE)
                {
                    LOGINFO("Triggering FrontPanel update by timer");
                    m_ledTimerIteration = 1;
                    m_ledState = state; 
                    m_ledTimer.Schedule(Core::Time::Now().Add(FRONT_PANEL_INTERVAL), m_ledInfo);
                }
            }
#else
            LOGERR("FrontPanel unsupported");
            response[PARAM_SUCCESS] = false;
            response[PARAM_ERROR] = "unsupported";
#endif
        }

        void Warehouse::internalReset(JsonObject& response)
        {
            bool isProd = false;

            if (0 == access(VERSION_FILE_NAME, R_OK))
            {
                std::ifstream file(VERSION_FILE_NAME);

                LOGWARN("%s: attempt to open file '%s' for read mode with result %s", __FUNCTION__
                        , VERSION_FILE_NAME, file.good() ? "sucess" : "failure");

                std::string line;
                while(std::getline(file, line))
                {
                    if (line.find("imagename:") != std::string::npos)
                    {
                        isProd = line.find("PROD") != std::string::npos;
                        break;
                    }
                }
            }

            if (isProd)
            {
                response[PARAM_SUCCESS] = false;
                response[PARAM_ERROR] = "version is PROD";
            }
            else
            {
#if defined(USE_IARMBUS) || defined(USE_IARM_BUS)
                std::string error;
                bool ok = RunScriptIARM(INTERNAL_RESET_SCRIPT, error);
                response[PARAM_SUCCESS] = ok;
                if (!ok)
                    response[PARAM_ERROR] = error;
#else
                response[PARAM_SUCCESS] = false;
                response[PARAM_ERROR] = "No IARMBUS";
#endif
            }
        }

        void Warehouse::lightReset(JsonObject& response)
        {
#if defined(USE_IARMBUS) || defined(USE_IARM_BUS)

            std::string script(LIGHT_RESET_SCRIPT);
            regex_t rx;
            regcomp(&rx, "(\\s+)([A-Z_][0-9A-Z_]*)(\\S*)", REG_EXTENDED);
            regmatch_t rm[4];

            int pos = 0;
            while (regexec(&rx, script.c_str() + pos, 4, rm, 0) == REG_NOERROR)
            {
                std::string var = script.substr(pos + rm[2].rm_so, rm[2].rm_eo - rm[2].rm_so);
                std::string replace;
                const char *envVar = getenv(var.c_str());

                std::string scmp;
                if ("SD_CARD_MOUNT_PATH" == var && (!envVar || 0 == *envVar))
                {

                    FILE *p = popen("cat  /proc/mounts | grep mmcblk0p1 | awk '{print $2}' ", "r");

                    if(!p)
                    {
                        LOGWARN("failed to run script to get SD_CARD_MOUNT_PATH");
                    }
                    else
                    {
                        char buf[256];
                        while(fgets(buf, sizeof(buf), p) != NULL)
                            scmp += buf;

                        scmp.erase(0, scmp.find_first_not_of(" \n\r\t"));
                        scmp.erase(scmp.find_last_not_of(" \n\r\t") + 1);

                        envVar = scmp.c_str();
                        pclose(p);
                    }
                }

                if (envVar && *envVar)
                {
                    replace += envVar;
                    replace += script.substr(pos + rm[3].rm_so, rm[3].rm_eo - rm[3].rm_so);
                    if (replace.size() > 0)
                    {
                        if ('/' == replace[0])
                        {
                            size_t nr = replace.find_first_not_of('/');
                            if (string::npos != nr && '*' != replace[nr]) // Check if that is not root dir
                                replace = script.substr(pos + rm[1].rm_so, rm[1].rm_eo - rm[1].rm_so) + replace;
                            else 
                                replace = "";
                        }
                        else
                            replace = script.substr(pos + rm[1].rm_so, rm[1].rm_eo - rm[1].rm_so) + replace;
                    }
                }
                script.replace(pos + rm[0].rm_so, rm[0].rm_eo - rm[0].rm_so, replace);
                pos += rm[0].rm_so + replace.size();
            }

            LOGWARN("lightReset: %s", script.c_str());

            std::string error;
            bool ok = RunScriptIARM(script, error);
            response[PARAM_SUCCESS] = ok;
            if (ok)
            {
                LOGWARN("lightReset succeeded");
            }
            else
            {
                LOGERR("lightReset failed. %s", error.c_str());
                response[PARAM_ERROR] = error;
            }
#else
            LOGERR("lightReset failed: No IARMBUS");
            response[PARAM_SUCCESS] = false;
            response[PARAM_ERROR] = "No IARMBUS";
#endif
        }

        void Warehouse::isClean(int age, JsonObject& response)
        {
            LOGINFO();

            std::ifstream customDataFile(CUSTOM_DATA_FILE);
            JsonArray existedObjects;

            if(!customDataFile)
            {
                LOGERR("Can't open file %s", CUSTOM_DATA_FILE);
                response[PARAM_SUCCESS] = false;
                response[PARAM_ERROR] = "Can't open file " CUSTOM_DATA_FILE;
                response["clean"] = false;
                response["files"] = existedObjects;
                return;
            }

            std::vector<std::string> listPathsToRemove;
            for( std::string line; getline( customDataFile, line ); )
            {
                Utils::String::trim(line);
                if (line.length() > 0)
                {
                    char firstChar = line[0];
                    if (firstChar != '#' && firstChar != '[')
                        listPathsToRemove.emplace_back(line);
                }
            }
            customDataFile.close();

            if (listPathsToRemove.size() == 0)
            {
                std::string msg = "file " CUSTOM_DATA_FILE " doesn't have any lines with paths";
                LOGERR("%s", msg.c_str());
                response[PARAM_ERROR] = msg;
                response[PARAM_SUCCESS] = false;
                response["files"] = existedObjects;
                return;
            }

            int totalPathsCounter = 0;
            for(auto &path : listPathsToRemove)
            {
                // if script's variable in path is empty, then skip it
                if (path.find('$') != std::string::npos)
                {
                    std::string variable;
                    std::string script = "echo '" + path + "' | sed -r \"s/([^$]*)([$\\{]*)([^$\\{\\}\\/]*)(.*)/\\3/\"";
                    variable = Utils::cRunScript(script.c_str());
                    Utils::String::trim(variable);

                    std::string value;
                    if (variable.length() > 0)
                    {
                        script = ". /etc/device.properties; echo \"$" + variable + "\"";
                        value = Utils::cRunScript(script.c_str());
                        Utils::String::trim(value);
                    }

                    if (value.length() == 0)
                    {
                        LOGWARN("path %d '%s' hasn't been tested, due to the empty value of '%s'", ++totalPathsCounter, path.c_str(), variable.c_str());
                        continue;
                    }

                    LOGINFO("variable '%s' has value '%s'", variable.c_str(), value.c_str());
                }

                if (std::find_if(path.begin(), path.end(), [](char c) { return c == '$' || c == '*' || c == '?' || c == '+'; } ) != path.end())
                {
                    // allow search recursively if path ends by '/*', otherwise, for cases like /*.ini, searching process will be done only for given path
                    std::string maxDepth = "-maxdepth 1 ";
                    int pathLength = path.length();
                    if (pathLength > 1 && path.find("/*", path.length() - 2) != std::string::npos)
                        maxDepth = "";

                    std::string script = ". /etc/device.properties; fp=\"";
                    script += path;
                    script += "\"; p=${fp%/*}; f=${fp##*/}; find $p -mindepth 1 ";
                    script += maxDepth;
                    script += "! -path \"*/\\.*\" -name \"$f\" 2>&1 | head -n 10";
                    std::string result = Utils::cRunScript(script.c_str());
                    Utils::String::trim(result);

                    if (result.length() > 1)
                    {
                        std::stringstream ss(result);
                        std::string line;
                        while(getline( ss, line, '\n' ))
                        {
                            if (age > -1)
                            {
                                bool objectExists = Utils::isFileExistsAndOlderThen(line.c_str(), (long)age);
                                if (objectExists)
                                    existedObjects.Add(line);

                                std::string strAge = std::to_string(age) + " seconds";
                                LOGINFO("object %d by path '%s' : '%s' %s", ++totalPathsCounter, path.c_str(), line.c_str()
                                        , objectExists
                                        ? (std::string("exists and was modified more than ") + strAge + " ago").c_str()
                                        : (std::string("doesn't exist or was modified in ") + strAge).c_str());
                            }
                            else
                            {
                                existedObjects.Add(line);
                                LOGINFO("object %d by path '%s' : '%s' exists", ++totalPathsCounter, path.c_str(), line.c_str());
                            }
                        }
                    }
                    else
                        LOGINFO("objects %d by path '%s' don't exist", ++totalPathsCounter, path.c_str());
                }
                else
                {
                    bool objectExists = Utils::isFileExistsAndOlderThen(path.c_str(), (long)age);
                    if (objectExists)
                        existedObjects.Add(path);

                    if (age > -1)
                    {
                        std::string strAge = std::to_string(age) + " seconds";
                        LOGINFO("object %d by path '%s' : '%s' %s", ++totalPathsCounter, path.c_str(), path.c_str()
                                , objectExists
                                ? (std::string("exists and was modified more than ") + strAge + " ago").c_str()
                                : (std::string("doesn't exist or was modified in ") + strAge).c_str());
                    }
                    else
                        LOGINFO("object %d by path '%s' %s", ++totalPathsCounter, path.c_str(), objectExists ? "exists" : "doesn't exist");
                }
            }

            LOGINFO("checked %d paths, found objects %d", totalPathsCounter, (int)existedObjects.Length());
            response[PARAM_SUCCESS] = true;
            response["files"] = existedObjects;
            response["clean"] = existedObjects.Length() == 0;
        }

        uint32_t Warehouse::resetDeviceWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFO();

            bool suppressReboot = false;
            if (parameters.HasLabel("suppressReboot"))
            {
                getBoolParameter("suppressReboot", suppressReboot);
            }

            resetDevice(suppressReboot);

            response[PARAM_SUCCESS] = true;
            returnResponse(true);
        }

        uint32_t Warehouse::getDeviceInfoWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFO();

            getDeviceInfo(response);

            response[PARAM_SUCCESS] = true;
            returnResponse(true);
        }

        uint32_t Warehouse::setFrontPanelStateWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFO();

            int state = 0;
            if (parameters.HasLabel("state"))
                getNumberParameter("state", state);

            setFrontPanelState(state, response);
            return Core::ERROR_NONE;
        }

        uint32_t Warehouse::internalResetWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFO();

            if (parameters.HasLabel("passPhrase") && parameters["passPhrase"].String() == "FOR TEST PURPOSES ONLY")
            {
                internalReset(response);
                return (Core::ERROR_NONE); 
            }
            else
            {
                response[PARAM_SUCCESS] = false;
                response[PARAM_ERROR] = "incorrect pass phrase";
                returnResponse(false);
            }
        }

        uint32_t Warehouse::lightResetWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFO();

            lightReset(response);
            return Core::ERROR_NONE;
        }

        uint32_t Warehouse::isCleanWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFO();

            int age;
            getDefaultNumberParameter("age", age, -1);

            isClean(age, response);
            return Core::ERROR_NONE;
        }


#ifdef HAS_FRONT_PANEL
        void Warehouse::onSetFrontPanelStateTimer()
        {
            SetFrontPanelLights(m_ledState, m_ledTimerIteration);
            ++m_ledTimerIteration;
            m_ledTimer.Schedule(Core::Time::Now().Add(FRONT_PANEL_INTERVAL), m_ledInfo);
        }

        uint64_t LedInfo::Timed(const uint64_t scheduledTime)
        {
            LOGINFO();
            uint64_t result = 0;
            m_warehouse->onSetFrontPanelStateTimer();
            return(result);
        }
#endif


    } // namespace Plugin
} // namespace WPEFramework
