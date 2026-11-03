#ifndef _TYPES
#define _TYPES

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct COLORStruct {
        short r;
        short g;
        short b;
        short ww;
        short cw;
        char RGBhex[8];
    } COLOR;


    typedef struct PrinterVaraiblesStruct{
        String parsedHMSlevel = "";
        uint64_t parsedHMScode = 0;            //8 bytes per code stored
        String gcodeState = "FINISH";           //Initialised to Finish so the logic doesn't 
                                                //assume a Print has just finished and needs 
                                                //to wait for a door interaction to continue
        int stage = 0;
        int overridestage = 999;
        bool printerledstate = true;
        bool hmsstate = false;
        bool online = false;
        bool finished = false;
        bool initalisedLEDs = false;
        //Time since
        unsigned long disconnectMQTTms = 0;        

        //PrinterType
        bool isP1Printer = false;               //Is this a P1 Printer without lidar or door switch
        //Door Monitoring
        bool useDoorSwtich = true;              //DoorSwitch to be used for Actions?
        bool doorOpen = false;                  // Current State of Door
        bool doorSwitchTriggered = false;       // Has door been closed twice within 6 seconds?
        bool waitingForDoor = false;            // Are we waiting for the door to be actuated?
        unsigned long lastdoorClosems = 0;      // Last time door was opened
        unsigned long lastdoorOpenms = 0;       // Last time door was closed
        bool chamberLightLocked = false;  // blocks replicate while true
        bool ledWasForcedByDoor = false;

        // Live print data (for the status page, /api/status and the progress effect)
        int percent = -1;                 // mc_percent, -1 = unknown
        int remainingMins = -1;           // mc_remaining_time
        int layer = -1;                   // layer_num
        int totalLayers = -1;             // total_layer_num
        float nozzleTemp = 0, nozzleTarget = 0;
        float bedTemp = 0, bedTarget = 0;
        float chamberTemp = 0;
        char jobName[64] = "";            // subtask_name

        // Filament colors (AMS slots 0-15 = unit*4 + slot, plus the external spool)
        char trayColor[16][7] = {};       // "RRGGBB", "" when the slot is empty or unknown
        char extColor[7] = "";
        int trayNow = 255;                // active slot: 0-15 AMS, 254 external spool, 255 none
        int trayTar = 255;                // slot the AMS is switching to (same numbering)
        // H2 series (H2D / H2C / H2S): the active slot is reported per nozzle instead of in tray_now
        bool perNozzleSlots = false;      // seen device.extruder.info[].snow
        int activeNozzle = 0;
        int nozzleNow[2] = {0xFFFF, 0xFFFF}; // unit << 8 | slot, per nozzle
        int nozzleTar[2] = {0xFFFF, 0xFFFF};
        char htColor[8][7] = {};          // AMS HT units (ids 128-135, one slot each)
        char virColor[2][7] = {};         // H2 external spools: [0] = id 255, [1] = id 254

        bool timelapse = false;           // printer is recording a timelapse (ipcam.timelapse)
        bool coolWaiting = false;         // print finished, holding the finish color until the bed cools
        unsigned long coolStartMs = 0;
        bool celebratePending = false;
        unsigned long filamentPreviewUntil = 0; // show a just-loaded spool's color until this time    // play the finish celebration once per finished print

        // Connection health
        bool mqttAuthFailed = false;      // printer refused the access code
        unsigned long lastMqttMsgMs = 0;  // last message of any kind from the printer
        uint32_t mqttConnects = 0;        // successful connects since boot
        uint32_t wifiDrops = 0;           // Wi-Fi drops since boot
    } PrinterVariables;
    PrinterVariables printerVariables;

    typedef struct SecurityVariables{
                // Security
        char HTTPUser[40] = "";             //http basic auth username
        char HTTPPass[40] = "";              //http basic auth password
    }SecurityVariables;
    SecurityVariables securityVariables;



    typedef struct GlobalVariablesStruct{
        char SSID[32];
        char APPW[64];
        String FWVersion = STRVERSION;
        String Host = "BLLED";
        bool started = false;
    } GlobalVariables;

    GlobalVariables globalVariables;

    typedef struct PrinterConfigStruct
    {
        char printerIP[16];             //BBLP IP Address - used for MQTT reports
        char accessCode[9];             //BBLP Access Code - used for MQTT reports
        char serialNumber[16];          //BBLP Serial Number - used for MQTT reports

        char BSSID[18];                 //Nominated AP to connect to (Useful if multiple accesspoints with same name)
        int brightness = 20;            //Brightness of LEDS - Default to 20% in case user use LED's that draw too much power for their PS
        bool rescanWiFiNetwork = false; //Scans available WiFi networks for strongest signal
        // LED Behaviour (Choose One)
        bool maintMode = false;         //White lights on, even if printer is unpowered
        bool maintMode_update = true;
        bool discoMode = false;         //Cycles through RGB colors slowly for 'pretty' timelapse movie
        bool discoMode_update = true;
        bool replicatestate = true;     //LED will be on if the BBPL Light is on
        bool replicate_update = true;     //LED will be on if the BBPL Light is on
        COLOR runningColor;             //Running Color (Default if no issues)
        bool testcolorEnabled = false;
        bool testcolor_update= true;    //When updateleds() is run, should the TEST LEDS be set?
        COLOR testColor;                //Test Color
        bool debugwifi = false;         //Changes LED to a color range that represents WiFi signal Strength        
        // Options
        bool finishindication = true;   //Enable / Disable
        COLOR finishColor;              //Set Finish Color
        bool finishExit = true;         //True = use Door / False = use Timer
        bool finish_check = false;    //When updateleds() is run, should the TEST LEDS be set?
        unsigned long finishStartms = 0;    // Time the finish countdown is measured from
        int finishTimeOut = 600000;     //300000 = 5 mins
        bool controlChamberLight = false;                //control chamber light

        //Inactivity Timout
        bool inactivityEnabled = true;
        bool isIdleOFFActive = false;       // Are the lights out due to inactivity Timeout?
        unsigned long inactivityStartms = 0;    // Time the inactivity countdown is measured from
        int inactivityTimeOut = 3600000;  // 1800000 = 30mins / 600000 = 10mins / 60000 = 1mins 
        // Debugging
        bool debuging = false;          //Debugging for all interactions through functions
        bool debugingchange = true;     //Default debugging level - to shows onChange
        bool mqttdebug = false;         //Writes each packet from BBLP to the serial log
        //Custom Colors for events using lidar
        COLOR stage14Color;
        COLOR stage1Color;
        COLOR stage8Color;
        COLOR stage9Color;
        COLOR stage10Color;
        // Customise LED Colors
        bool errordetection = true;     //Utilises Error Colors when BBLP give an error
        COLOR wifiRGB;                  
        COLOR pauseRGB;
        COLOR firstlayerRGB;
        COLOR nozzleclogRGB;
        COLOR hmsSeriousRGB;
        COLOR hmsFatalRGB;
        COLOR filamentRunoutRGB;
        COLOR frontCoverRGB;
        COLOR nozzleTempRGB;
        COLOR bedTempRGB;
        // HMS Error Handling
        String hmsIgnoreList; // comma-separated list of HMS_XXXX_XXXX_XXXX_XXXX codes to ignore

        // Effects (see LedFx in leds.h: 0 solid, 1 breathe, 2 blink)
        int fadeMs = 600;               // Cross-fade time between colors
        uint8_t pauseFx = 1;            // Pause / first-layer / clog: breathe
        uint8_t errorFx = 2;            // Errors and HMS faults: blink
        uint8_t finishFx = 1;           // Print finished: breathe
        bool progressFx = false;        // While printing, blend running color -> finish color by %
        bool filamentColorFx = false;   // While printing, glow in the active filament's color
        bool timelapseSteady = true;    // No pulsing while a timelapse is recording (it would flicker)
        bool heatGlowFx = false;        // While heating, blend blue -> orange with temperature
        bool celebrateFx = false;       // Short rainbow sweep when a print finishes
        bool coolDownEnabled = false;   // Hold the finish color until the bed is cool enough to remove the part
        int coolDownTemp = 35;          // deg C
        bool doorWorkLight = false;     // Bright white while the door is open
        bool almostDoneEnabled = false; // Shift toward the finish color in the last minutes of a print
        int almostDoneMins = 5;
        int calR = 100, calG = 100, calB = 100; // color balance: strength of each RGB channel, %
        bool firstLayerSpotlight = true; // Full-brightness white for the first layer (never during lidar scans)
        bool needsYouEnabled = true;    // Paused and waiting on you (not paused by you): its own color
        COLOR needsYouRGB;
        COLOR amsErrorRGB;              // AMS problems (AMS lost, AMS errors)
        bool idleDimEnabled = true;     // When the idle timer runs out, dim first instead of going dark
        int idleDimLevel = 15;          // % of normal brightness
        int idleOffMins = 60;           // then turn off after this many more minutes (0 = stay dim)

        // Quiet hours: dim (or turn off) the strip during a nightly window
        bool quietEnabled = false;
        int quietStart = 23 * 60;       // minutes after midnight
        int quietEnd = 7 * 60;
        int quietBrightness = 10;       // % during quiet hours, 0 = off
        char tz[48] = "MST7MDT,M3.2.0,M11.1.0"; // POSIX time zone for the clock (US Mountain)


    } PrinterConfig;

    PrinterConfig printerConfig;

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif