#ifndef FED4_H
#define FED4_H

#include <Arduino.h>
#include <functional>
#include <string>

#include <Adafruit_NeoPixel.h>
#include <Adafruit_SharpMem.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#include <RTCZero.h>
#include <SdFat.h>
#include <Stepper.h>
#include <WDTZero.h>

#include "Menu.h"

#define OLD_WELL false

constexpr uint16_t LP_AWAKE_PERIOD = 30; // seconds

constexpr uint16_t DISPLAY_H = 144; // pxls
constexpr uint16_t DISPLAY_W = 168; // pxls

constexpr size_t ROW_MAX_LEN        = 500;
constexpr size_t FILE_RAM_BUFF_SIZE = 1024; // BYTES
constexpr size_t FILE_PREALLOC_SIZE = 25 * 1024UL * 1024UL; // 25MB 

constexpr uint16_t STEPS = 2048;

namespace FED4Pins {
    constexpr uint8_t NEOPXL    = A1;
    constexpr uint8_t GRN_LED   = 8;
    constexpr uint8_t WELL      = 1;
    constexpr uint8_t LFT_POKE  = 6;
    constexpr uint8_t RGT_POKE  = 5;
    constexpr uint8_t BUZZER    = 0;
    constexpr uint8_t VBAT      = A7;
    constexpr uint8_t CARD_SEL  = 4;
    constexpr uint8_t BNC_OUT   = A0;
    constexpr uint8_t SHRP_SCK  = 12;
    constexpr uint8_t SHRP_MOSI = 11;
    constexpr uint8_t SHRP_CS   = 10;
    constexpr uint8_t MTR_EN    = 13;
    constexpr uint8_t MTR_1     = A2;
    constexpr uint8_t MTR_2     = A3;
    constexpr uint8_t MTR_3     = A4;
    constexpr uint8_t MTR_4     = A5;
}

namespace Mode {
    constexpr int8_t FR      = 0;
    constexpr int8_t VI      = 1;
    constexpr int8_t CHANCE  = 2;
    constexpr int8_t OTHER   = -1;
};

namespace ActiveSensor {
    constexpr uint8_t LEFT    = 0;
    constexpr uint8_t RIGHT   = 1;
    constexpr uint8_t BOTH    = 2;
};
 

namespace EventMsg {
    constexpr const char* LEFT     = "Left Poke";
    constexpr const char* RIGHT    = "Right Poke";
    constexpr const char* PEL      = "Dropped Pellet";
    constexpr const char* WELL     = "Well Cleared";
    constexpr const char* SET_VI   = "Set VI";
    constexpr const char* RESET    = "Reset Device";
    constexpr const char* WTD_RTS  = "WatchDog Reset Device";
    constexpr const char* NONE     = "";
}

struct Event {
    DateTime time;
    const char* message;
};

namespace ErrorMsg {
    constexpr const char* JAM = "JAM OR NO PELLETS"; 
}


class FED4 {
    public:
    static FED4* instance;  
    
    FED4() :
        display(
            FED4Pins::SHRP_SCK, FED4Pins::SHRP_MOSI, 
            FED4Pins::SHRP_CS, DISPLAY_H, DISPLAY_W
        ),
        stepper(
            STEPS, FED4Pins::MTR_1, FED4Pins::MTR_2, 
            FED4Pins::MTR_3, FED4Pins::MTR_4
        )
        // strip(10, FED4Pins::NEOPXL, NEO_GRBW + NEO_KHZ800) 
    {
        watch_dog.attachShutdown(wtd_shut_down);

        rtc.begin();
        if (rtc.lostPower()) {
            rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        }
        stepper.setSpeed(7);
        
        display.begin();
        display.clearDisplay();
        display.setRotation(3);
        display.refresh();
    }


    // ==== Hardware Objects ====
    SdFat sd;
    RTC_PCF8523 rtc;
    RTCZero rtcZero;
    Adafruit_SharpMem display;
    Stepper stepper;
    // Adafruit_NeoPixel strip;

    
    // ==== Pulbic Flags ====
    bool ignorePokes = false;
    
    
    // ==== Device State ====
    uint16_t leftPokeCount = 0;
    uint16_t rightPokeCount = 0;
    uint16_t pelletsDispensed = 0;
    
    uint8_t deviceNumber = 0;
    uint8_t animal = 0;
    uint8_t activeSensor = ActiveSensor::BOTH;
    uint8_t leftReward = 1;
    uint8_t rightReward = 1;
    bool feedWindow = true;
    uint8_t windowStart = 9;
    uint8_t windowEnd = 12;
    
    SdFile logFile;
    
    // Mode Specific
    int8_t mode = Mode::VI;
    uint8_t ratio = 1;
    uint8_t viAvg = 30;
    float viSpread = 0.75;
    uint16_t viCountDown = 0;
    uint32_t feedUnixT = 0;
    bool viSet = false;
    float chance = 0.5;
    
    
    // ==== API ====
    void begin();
    void run();
    void sleep();
    
    void feed(int pellets = 1, bool wait = true);
    void rotateWheel(int degrees);
    
    void loadConfig();
    void saveConfig();
    
    bool getLeftPoke();
    bool getRightPoke(); 
    bool getWellStatus();
    
    void initSD();
    void showSdError();
    void initLogFile();
    void logEvent(Event e);
    void logError(String str);
    
    void updateDisplay(bool timeOnly = false);
    void displayLayout();
    void print(String str, uint8_t size = 2);
    void drawDateTime();
    void drawBateryCharge();
    void drawStats();
    
    void makeNoise(int duration = 300);
    
    void runConfigMenu();
    void runFRMenu();
    void runVIMenu();
    void runChanceMenu();
    std::function<void()> runOtherModeMenu = nullptr;
    
    bool checkCondition();
    bool checkFRCondition();
    bool checkVICondition();
    bool checkChanceCondition();
    std::function<bool()> checkOtherCondition = nullptr;
    
    bool checkFeedingWindow();
    void setLightCue();

    int getViCountDown();
    
    DateTime getDateTime();
    int getBatteryPercentage();
    
    private:
    // ==== InternalFlags ====
    volatile bool _sleep_mode     = false;
    
    volatile bool _left_poke      = false;
    volatile bool _right_poke     = false;
    volatile bool _pellet_dropped = false;
    volatile bool _pellet_in_well = false;
    
    volatile bool _jam_error      = false;
    
    // Timing
    volatile bool _left_poke_started      = false;
    volatile bool _right_poke_started     = false;
    volatile unsigned long _startT_left_poke     = 0;
    volatile unsigned long _startT_right_poke    = 0;
    volatile unsigned long _dT_left_poke         = 0;
    volatile unsigned long _dT_right_poke        = 0;
    
    
    // ==== Internal State ====
    int _reward;
    
    // Log Memory
    size_t _log_buffer_pos = 0;
    char _log_buffer[FILE_RAM_BUFF_SIZE];
    unsigned long _last_flush = 0;
    void write_to_log(char row[ROW_MAX_LEN], bool forceFlush=false);
    void flush_to_sd();
    
    
    // ==== Interrupts ====
    volatile bool _interrupt_enabled = true;
    void start_interrupts();
    void pause_interrupts();

    unsigned long _last_pellet_t;
    
    void left_poke_handler();
    void right_poke_handler();
    void alarm_handler();
    void well_handler();
    
    // Static Interrupt Service Routinesd
    static void left_poke_IRS();
    static void right_poke_IRS();
    static void well_ISR();
    static void alarm_ISR();
    
    // SD File CallBack
    static void dateTime(uint16_t* date, uint16_t* time);
    
    
    // ==== Watch Dog ====
    WDTZero watch_dog;
    uint32_t _wtd_timeout = WDT_SOFTCYCLE2M;
    static void wtd_shut_down();
    void wtd_restart();
};

#endif