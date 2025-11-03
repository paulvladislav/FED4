#include "FED4.h"

FED4 *FED4::instance = nullptr;

void __delay(uint32_t ms) {
    unsigned long startT = millis();
    while(millis() - startT > ms);
}

void HardFault_Handler(void) {
    __asm("BKPT #0");
    while(1){}
}

void FED4::begin() {
    instance = this;

    // Motor pins
    pinMode(FED4Pins::MTR_EN, OUTPUT);
    pinMode(FED4Pins::MTR_1, OUTPUT);
    pinMode(FED4Pins::MTR_2, OUTPUT);
    pinMode(FED4Pins::MTR_3, OUTPUT);
    pinMode(FED4Pins::MTR_4, OUTPUT);
    
    // Input pins
    pinMode(FED4Pins::LFT_POKE, INPUT_PULLUP);
    pinMode(FED4Pins::RGT_POKE, INPUT_PULLUP);
    pinMode(FED4Pins::WELL, INPUT);
    digitalWrite(FED4Pins::WELL, HIGH);
    
    // Interrupts
    attachInterrupt(digitalPinToInterrupt(FED4Pins::LFT_POKE), left_poke_IRS, CHANGE);
    attachInterrupt(digitalPinToInterrupt(FED4Pins::RGT_POKE), right_poke_IRS, CHANGE);
    attachInterrupt(digitalPinToInterrupt(FED4Pins::WELL), well_ISR, CHANGE);
    
    // Wakeup sources
    EIC->WAKEUP.reg |= (1 << 4);   // FED4Pins::LFT_POKE
    EIC->WAKEUP.reg |= (1 << 15);  // FED4Pins::RGT_POKE
    EIC->WAKEUP.reg |= (1 << 16);  // RTC peripheral
    
    // Clock setup
    SYSCTRL->XOSC32K.reg |= (SYSCTRL_XOSC32K_RUNSTDBY | SYSCTRL_XOSC32K_ONDEMAND);
    GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID(GCM_EIC) |
    GCLK_CLKCTRL_CLKEN |
    GCLK_CLKCTRL_GEN_GCLK1;
    while (GCLK->STATUS.bit.SYNCBUSY);
    
    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk; // Enable deep sleep mode

    rtc.begin();
    if (rtc.lostPower()) {
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }

    rtcZero.begin();
    rtcZero.setTime(rtc.now().hour(), rtc.now().minute(), rtc.now().second());
    rtcZero.setDate(rtc.now().day(), rtc.now().month(), rtc.now().year() - 2000);
    uint8_t alarmSeconds = rtcZero.getSeconds() + LP_AWAKE_PERIOD;
    if (alarmSeconds >= 59) {
        alarmSeconds -= 60;
    }
    rtcZero.setAlarmTime(0, 0, alarmSeconds);
    rtcZero.attachInterrupt(alarm_ISR);
    rtcZero.enableAlarm(RTCZero::MATCH_SS);
    
    randomSeed(rtc.now().unixtime());
    
    // digitalWrite(FED4Pins::MTR_EN, HIGH);
    // __delay(2);
    // strip.begin();
    // strip.clear();
    // strip.show();
    // digitalWrite(FED4Pins::MTR_EN, LOW);
    
    SdFile::dateTimeCallback(dateTime);
    initSD();
    
    loadConfig();
    
    if (PM->RCAUSE.reg & PM_RCAUSE_WDT) {
        wtd_restart();
        displayLayout();
        return;
    }
    
    menu_display = &display;
    menu_rtc = &rtc;
    runConfigMenu();
    switch (mode) {
    case Mode::FR:
        runFRMenu();
        break;
    case Mode::VI:
        runVIMenu();
        break;
    case Mode::CHANCE:
        runChanceMenu();
        break;
    case Mode::OTHER:
        runOtherModeMenu();
        break;
    default:
        break;
    }

    initLogFile();
    
    saveConfig();
    displayLayout();
    
    watch_dog.setup(_wtd_timeout);
}

void FED4::run() {
    // setLightCue();

    updateDisplay();    
    
    if (checkCondition()) {
        feed(_reward);
    }
    
    if (!checkFeedingWindow()) {
        sleep();
    }

    watch_dog.clear();
}

void FED4::sleep() {
    _sleep_mode = true;
    __DSB();
    while(_sleep_mode) {
        __WFI();
    }
}

void FED4::feed(int pellets, bool wait) {
    if (_jam_error) return;

    _pellet_dropped = false;

    long startOfFeed;
    for (int i = 0; i < pellets; i++) {
        startOfFeed = millis();
#if OLD_WELL
        while(_pellet_in_well == false)
#else
        while (getWellStatus() == false)
#endif
        {
            long deltaT = millis() - startOfFeed;
            if (deltaT < 15000)
            {
                rotateWheel(5);
            }
            else if (deltaT < 30000)
            {
                rotateWheel(-3);
                rotateWheel(5);
            }
            else if (deltaT < 90000)
            {
                rotateWheel(-10);
                rotateWheel(15);
            }
            else {
                logError("Clogged or No Pellets");
                _jam_error = true;
                updateDisplay();
                return;
            }
        }

        pelletsDispensed++;
        Event event = {
            .time = getDateTime(),
            .message = EventMsg::PEL
        };
        logEvent(event);
        __delay(200);

#if OLD_WELL
        while(_pellet_in_well);
#endif
    }

    _left_poke = false;
    _right_poke = false;

    updateDisplay();
}

void FED4::rotateWheel(int degrees) {
    digitalWrite(FED4Pins::MTR_EN, HIGH);
    __delay(2);

    int steps = (STEPS * degrees / 360);
    stepper.step(steps);

    digitalWrite(FED4Pins::MTR_EN, LOW);
}

void FED4::loadConfig() {
    if (!sd.exists("CONFIG.json")) return;

    File configFile = sd.open("CONFIG.json", FILE_READ);
    JsonDocument config;
    deserializeJson(config, configFile);
    
    deviceNumber = config["device number"]; 
    animal = config["animal"];

    if (config["mode"]["name"] == "FR") {
        mode = Mode::FR;
        ratio = config["mode"]["ratio"];
    } else if (config["mode"]["name"] == "VI") {
        mode = Mode::VI;
        viAvg = config["mode"]["avg"];
        viSpread = config["mode"]["spread"];
    } else if (config["mode"]["name"] == "CHANCE") {
        mode = Mode::CHANCE;
        chance = config["mode"]["chance"];
    }

    if (config["active sensor"] == "left") {
        activeSensor = ActiveSensor::LEFT;
    } else if (config["active sensor"] == "right") {
        activeSensor = ActiveSensor::RIGHT;
    } else if (config["active sensor"] == "both") {
        activeSensor = ActiveSensor::BOTH;
    }

    leftReward = config["reward"]["left"];
    rightReward = config["reward"]["right"];
    if (config["reward"]["window"] == true) {
        feedWindow = true;
        windowStart = config["reward"]["time"]["start"];
        windowEnd = config["reward"]["time"]["end"];
    } else {
        feedWindow = false;
    }

    configFile.close();
}

void FED4::saveConfig() {
    sd.remove("CONFIG.json");
    File configFile = sd.open("CONFIG.json", FILE_WRITE);
    JsonDocument config;

    config["device number"] = deviceNumber;
    config["animal"] = animal;

    switch (mode) {
    case Mode::FR:
        config["mode"]["name"] = "FR";
        config["mode"]["ratio"] = ratio;
        break;

    case Mode::VI:
        config["mode"]["name"] = "VI";
        config["mode"]["avg"] = viAvg;
        config["mode"]["spread"] = viSpread;
        break;

    case Mode::CHANCE:
        config["mode"]["name"] = "CHANCE";
        config["mode"]["chance"] = chance;
    
    default:
        break;
    }

    switch (activeSensor) {
    case ActiveSensor::LEFT:
        config["active sensor"] = "left";
        break;

    case ActiveSensor::RIGHT:
        config["active sensor"] = "right";
        break;

    default:
        break;
    }

    config["reward"]["left"] = leftReward;
    config["reward"]["right"] = rightReward;
    if (feedWindow) {
        config["reward"]["window"] = true;
        config["reward"]["time"]["start"] = windowStart;
        config["reward"]["time"]["end"] = windowEnd;
    } else {
        config["reward"]["window"] = false;
    }
    
    serializeJson(config, configFile);
    configFile.close();
}

bool FED4::getLeftPoke() {
    if (_left_poke)
    {
        _left_poke = false;
        return true;
    }
    return false;
}

bool FED4::getRightPoke() {
    if (_right_poke)
    {
        _right_poke = false;
        return true;
    }
    return false;
}

bool FED4::getWellStatus() {
    if(_pellet_dropped) {
        _pellet_dropped = false;
        return true;
    }
    return false;
}

void FED4::initSD() {
    digitalWrite(FED4Pins::MTR_EN, LOW);

    while (!sd.begin(FED4Pins::CARD_SEL, SD_SCK_MHZ(4)))
    {
        showSdError();
    }
}

void FED4::showSdError() {
    display.setTextSize(2);
    display.setTextColor(BLACK);
    display.clearDisplay();
    display.setCursor(20, 40);
    display.println("   Check");
    display.setCursor(10, 60);
    display.println("  SD Card!");
    display.refresh();

    delay(1000);
    display.clearDisplay();
    display.refresh();
    delay(1000);
}

void FED4::initLogFile() {   
    digitalWrite(FED4Pins::MTR_EN, LOW);
    char fileName[30] = "";

    DateTime now = getDateTime();
    
    strcat(fileName, "FED");
    if (deviceNumber < 10) strcat(fileName, "0");
    strcat(fileName, String(deviceNumber).c_str());
    strcat(fileName, "-");
    if (animal < 100) strcat(fileName, "0");
    if (animal < 10) strcat(fileName, "0");
    strcat(fileName, String(animal).c_str());
    strcat(fileName, "_");
    if (now.day() < 10) strcat(fileName, "0");
    strcat(fileName, String(now.day()).c_str());
    strcat(fileName, "-");
    if (now.month() < 10) strcat(fileName, "0");
    strcat(fileName, String(now.month()).c_str());
    strcat(fileName, "-");
    if (now.year() % 100 < 10) strcat(fileName, "0");
    strcat(fileName, String(now.year() % 100).c_str());
    strcat(fileName, "_");
    strcat(fileName, "01.csv");
    
    // FED01-001_06-03-25_01.csv
    
    int fileIndex = 1;
    while (sd.exists(fileName))
    {
        fileIndex++;
        fileName[19] = '0' + fileIndex / 10;
        fileName[20] = '0' + fileIndex % 10;
    }
    
    logFile.open(fileName, FILE_WRITE);
    logFile.createContiguous(fileName, FILE_PREALLOC_SIZE);
    logFile.rewind();

    char header[500] = "";
    strcat(header, "TimeStamp,");
    strcat(header, "Device Number,");
    strcat(header, "Animal,");
    strcat(header, "Mode,");
    strcat(header, "Window Start,");
    strcat(header, "Window End,");
    strcat(header, "In Window,");
    strcat(header, "Event,");
    strcat(header, "Active Sensor,");
    strcat(header, "Left Reward,");
    strcat(header, "Right Reward,");
    strcat(header, "Left Poke Count,");
    strcat(header, "Right Poke Count,");
    strcat(header, "Pellet Count");


    switch (mode) {
    case Mode::VI:
        strcat(header, ",VI Count Down");
        break;

    case Mode::FR:
        strcat(header, ",Ratio");
        break;

    case Mode::CHANCE:
        strcat(header, ",Chance");
        break;
    
    default:
        break;
    }

    strcat(header, "\n");

    pause_interrupts();
    logFile.print(header);
    logFile.flush();
    start_interrupts();
}

void FED4::logEvent(Event e) {
    char row[ROW_MAX_LEN] = "";
    DateTime now = getDateTime();
    
    char date[20];
    sprintf(date, "%d/%d/%d ", now.day(), now.month(), now.year() % 1000);
    char  time[20];
    sprintf(time, "%d:%d:%d", now.hour(), now.minute(), now.second());
    strcat(row, date);
    strcat(row, time);
    strcat(row, ",");
    
    char deviceNumber_str[8];
    snprintf(deviceNumber_str, sizeof(deviceNumber_str), "%d", deviceNumber%100);
    strcat(row, deviceNumber_str);
    strcat(row, ",");

    char animal_str[8];
    sniprintf(animal_str, sizeof(animal_str), "%d", animal);
    strcat(row, animal_str);
    strcat(row, ",");

    char mode_str[10];
    switch (mode) {
    case Mode::FR:
        sprintf(mode_str, "FR");
        break;
        
    case Mode::VI:
        sprintf(mode_str, "VI");
        break;
        
    case Mode::CHANCE:
        sprintf(mode_str, "CHANCE");
        break;

    default :
        sprintf(mode_str, "OTHER");
        break;
    }
    strcat(row, mode_str);
    strcat(row, ",");

    if (feedWindow) {
        char window_start_str[8];
        char window_end_str[8];
        char in_window_str[4];
        sprintf(window_start_str, "%d", windowStart);
        sprintf(window_end_str, "%d", windowEnd);
        if (checkFeedingWindow()) {
            sprintf(in_window_str, "1");
        }
        else {
            sprintf(in_window_str, "0");
        }
        strcat(row, window_start_str);
        strcat(row, ",");
        strcat(row, window_end_str);
        strcat(row, ",");
        strcat(row, in_window_str);
        strcat(row, ",");
    }
    else {
        strcat(row, "null,null,null,");
    }

    strcat(row, e.message);
    strcat(row, ",");

    char activeSensor_str[10];
    switch (activeSensor) {
    case ActiveSensor::BOTH:
        sprintf(activeSensor_str, "Both");
        break;
    
    case ActiveSensor::LEFT:
        sprintf(activeSensor_str, "Left");
        break;
    
    case ActiveSensor::RIGHT:
        sprintf(activeSensor_str, "Right");
        break;
    }
    strcat(row, activeSensor_str);
    strcat(row, ",");

    char leftReward_str[5];
    char rightReward_str[5];
    switch (activeSensor) {
    case ActiveSensor::LEFT:
        snprintf(leftReward_str, sizeof(leftReward_str), "%d", leftReward);
        snprintf(rightReward_str, sizeof(rightReward_str), "null");
        break;
        
    case ActiveSensor::RIGHT:
        snprintf(leftReward_str, sizeof(leftReward_str), "null");
        snprintf(rightReward_str, sizeof(rightReward_str), "%d", rightReward);
        break;
        
    case ActiveSensor::BOTH:
        snprintf(leftReward_str, sizeof(leftReward_str), "%d", leftReward);
        snprintf(rightReward_str, sizeof(rightReward_str), "%d", rightReward);
        break;    
    }
    strcat(row, leftReward_str);
    strcat(row, ",");
    strcat(row, rightReward_str);
    strcat(row, ",");
    
    char leftPokeCount_str[8];
    snprintf(leftPokeCount_str, sizeof(leftPokeCount_str), "%d", leftPokeCount);
    char rightPokeCount_str[8];
    snprintf(rightPokeCount_str, sizeof(rightPokeCount_str), "%d", rightPokeCount);
    strcat(row, leftPokeCount_str);
    strcat(row, ",");
    strcat(row, rightPokeCount_str);
    strcat(row, ",");
    
    char pelletsDispensed_str[8];
    snprintf(pelletsDispensed_str, sizeof(pelletsDispensed_str), "%d", pelletsDispensed);
    strcat(row, pelletsDispensed_str);
    
    switch (mode) {
    case Mode::VI:
        char viCountDown_str[10];
        sprintf(viCountDown_str, "%d", viCountDown);
        strcat(row, ",");
        strcat(row, viCountDown_str);
        break;

    case Mode::FR:
        char ratio_str[10];
        sprintf(ratio_str, "%d", ratio);
        strcat(row, ",");
        strcat(row, ratio_str);
        break;

    case Mode::CHANCE:
        char chance_str[8];
        sprintf(chance_str, "%.2f", chance);
        strcat(row, ",");
        strcat(row, chance_str);
        break;
    
    default:
        break;
    }
    
    strcat(row, "\n");
    
    write_to_log(row);
}

void FED4::logError(String str) {
    char errorMsg[100] = "";
    snprintf(errorMsg, sizeof(errorMsg), "Error: %s", str.c_str());
    Event event = {
        .time = getDateTime(),
        .message = (const char *)errorMsg
    };
    logEvent(event);
}

void FED4::updateDisplay(bool statusOnly) {
    pause_interrupts();
    
    digitalWrite(FED4Pins::SHRP_CS, LOW);
    digitalWrite(FED4Pins::CARD_SEL, HIGH);
    
    if (!statusOnly) {
        drawStats();
    }
    
    drawDateTime();
    drawBateryCharge();

    if (_jam_error) {
        print("ERROR: JAM OR NO PELLETS");
    }

    display.refresh();

    start_interrupts();
}

void FED4::displayLayout() {
    display.clearDisplay();

    // draw the top line
    display.drawLine(5, 20, DISPLAY_W - 5, 20, BLACK);

    display.setTextSize(2);
    display.setTextColor(BLACK);

    display.setCursor(4, 26);
    display.print("Left: ");
    display.setCursor(4, 46);
    display.print("Right: ");
    display.setCursor(4, 66);
    display.print("Pellets: ");
    if (mode == Mode::VI)
    {
        display.setCursor(4, 86);
        display.print("VI CD: ");
    }

    display.setTextSize(1);
    display.setCursor(4, 124);
    if (mode == Mode::FR)
        display.print("Fixed Ratio");
    else if (mode == Mode::VI)
        display.print("Variable Interval");
    display.setCursor(4, 134);
    char logFileName[30];
    logFile.getName(logFileName, 30);
    display.print(logFileName);

    updateDisplay(true);
}

void FED4::print(String str, uint8_t size) {
    display.setTextSize(size);
    display.fillRect(2, 106, DISPLAY_W - 4, 16, WHITE);
    display.setTextColor(BLACK);
    display.setCursor(4, 106);
    display.print(str);
    display.refresh();
}

void FED4::drawDateTime() {
    DateTime now = getDateTime();

    display.setTextSize(1);
    display.setTextColor(BLACK);

    // cover up the date and time
    display.fillRect(2, 6, 95, 10, WHITE);

    display.setCursor(4, 8);
    if (now.day() < 10)
        display.print(0);
    display.print(now.day());
    display.print("/");
    if (now.month() < 10)
        display.print(0);
    display.print(now.month());
    display.print("/");
    display.print(now.year() % 1000);

    display.setCursor(65, 8);
    if (now.hour() < 10)
        display.print(0);
    display.print(now.hour());
    display.print(":");
    if (now.minute() < 10)
        display.print(0);
    display.print(now.minute());
}

void FED4::drawBateryCharge() {
    // cover up the battery symbol
    display.fillRect(129, 3, 37, 14, WHITE);

    // draw the battery symbol
    display.drawRect(134, 5, 31, 12, BLACK);
    display.fillRect(131, 8, 3, 6, BLACK);

    int batteryPercentage = getBatteryPercentage();

    if (batteryPercentage > 75)
    {
        display.fillRect(136, 7, 6, 8, BLACK);
    }
    if (batteryPercentage > 50)
    {
        display.fillRect(136 + 7, 7, 6, 8, BLACK);
    }
    if (batteryPercentage > 25)
    {
        display.fillRect(136 + 14, 7, 6, 8, BLACK);
    }
    if (batteryPercentage > 10)
    {
        display.fillRect(136 + 21, 7, 6, 8, BLACK);
    }
    if (batteryPercentage <= 10)
    {
        display.setTextSize(2);
        display.fillRect(144, 4, 12, 16, WHITE);
        display.setCursor(145, 4);
        display.print("!");
    }
}

void FED4::drawStats() {
    display.setTextSize(2);

    display.fillRect(98, 24, 100, 80, WHITE);

    display.setTextSize(2);

    display.setCursor(100, 26);
    display.print(leftPokeCount);
    display.setCursor(100, 46);
    display.print(rightPokeCount);
    display.setCursor(100, 66);
    display.print(pelletsDispensed);

    if (mode == Mode::VI) {
        display.setCursor(100, 86);
        if (viCountDown >= 0) {
            display.print(viCountDown);
        }
    }
}

void FED4::makeNoise(int duration) {
    for (int i = 0; i < duration / 50; i++)
    {
        tone(FED4Pins::BUZZER, random(50, 250), 50);
        __delay((uint32_t)(duration / 50));
    }
}

void FED4::runConfigMenu() {
    ignorePokes = true;

    Menu configMenu = Menu();

    configMenu.add("Time", new ClockMenu());
    configMenu.add("Animal", &animal, 0, 999, 1);

    const char* modes[] = {"FR", "VI", "%"};
    configMenu.add("Mode", &mode, modes, 3);

    const char* sensors[] = {"L", "R", "L&R"};
    configMenu.add("Sensor", &activeSensor, sensors, 3);

    configMenu.add("L Rew", &leftReward, 0, 255, 1);
    configMenu.add("R Rew", &rightReward, 0, 255, 1);

    configMenu.add("Rew Win", &feedWindow);
    configMenu.add("Rew Beg", &windowStart, 0, 23, 1);
    configMenu.add("Rew End", &windowEnd, 0, 23, 1);
    
    int batteryLevel = getBatteryPercentage();
    configMenu.run(batteryLevel); 

    ignorePokes = false;
}

void FED4::runFRMenu() {
    ignorePokes = true;

    Menu frMenu = Menu();
    frMenu.add("Ratio", &ratio, 1, 10, 1);
    frMenu.run();
    
    ignorePokes = false;
}

void FED4::runVIMenu() {
    ignorePokes = true;

    Menu viMenu = Menu();
    viMenu.add("Avg T", &viAvg, 1, 120, 5);
    viMenu.add("Spread", &viSpread, 0.0, 1.0, 0.05);
    viMenu.run();

    ignorePokes = false;
}

void FED4::runChanceMenu() {
    ignorePokes = true;

    Menu chanceMenu = Menu();
    chanceMenu.add("Chance", &chance, 0.0, 1.0, 0.05);
    chanceMenu.run();

    ignorePokes = false;
}

bool FED4::checkCondition() {
    if (feedWindow && !checkFeedingWindow()) {
        return false;
    }

    bool conditionMet;
    switch (mode) {
    case Mode::FR:
        conditionMet = checkFRCondition();
        break;

    case Mode::VI:
        conditionMet = checkVICondition();
        break;
    
    case Mode::CHANCE:
        conditionMet = checkChanceCondition();
        break;
    
    default:
        if(checkOtherCondition == nullptr) {
            conditionMet = false;
        }
        else {
            conditionMet = checkOtherCondition();
        }
        break;  
    }

    return conditionMet;
}

bool FED4::checkFRCondition() {
    bool conditionMet = false;
    switch (activeSensor) {
    case ActiveSensor::BOTH:
        if ( (leftPokeCount + rightPokeCount) % ratio == 0 ) {
            if (getLeftPoke()) {
                _reward = leftReward;
                conditionMet = true;
            }
            if (getRightPoke()) {
                _reward = rightReward;
                conditionMet = true;
            }
        }
        break;

    case ActiveSensor::LEFT:
        if (getLeftPoke() && leftPokeCount % ratio == 0) {
            _reward = leftReward;
            conditionMet = true;
        }
        break;

    case ActiveSensor::RIGHT:
        if(getRightPoke() && rightPokeCount %ratio == 0) {
            _reward = rightReward;
            conditionMet = true;
        }
        break;
    }
    
    return conditionMet;
}

bool FED4::checkVICondition() {
    if (viSet) {
        if (viCountDown <= 0) {
            viSet = false;
            viCountDown = 0;
            return true;
        }
        else {
            viCountDown  = (int)(feedUnixT - getDateTime().unixtime());
        }
    }
    else {
        bool pokedLeft = getLeftPoke();
        bool pokedRight = getRightPoke();

        bool conditionMet = false;
        switch (activeSensor) {
        case ActiveSensor::BOTH:
            if (pokedLeft || pokedRight) {
                conditionMet = true;
            }
            break;
        
        case ActiveSensor::LEFT:
            if (pokedLeft) {
                conditionMet = true;
            }
            break;

        case ActiveSensor::RIGHT:
            if (pokedRight) {
                conditionMet = true;
            }
        }

        if (conditionMet) {
            viCountDown = getViCountDown();
            viSet = true;

            if (pokedLeft) {
                _reward = leftReward;
            }
            else if (pokedRight) {
                _reward = rightReward;
            }

            Event e = Event {
                .time = getDateTime(),
                .message = EventMsg::SET_VI
            };
            logEvent(e);

            feedUnixT = getDateTime().unixtime() + viCountDown;
        }
    }

    return false;
}

bool FED4::checkChanceCondition() {
    int r = random(0, 100);
    if (
        getLeftPoke()
        && ( activeSensor == ActiveSensor::LEFT || activeSensor == ActiveSensor::BOTH )
        && r <= int(chance * 100)
    ) {
        _reward = leftReward;
        return true;
    }
    else if (
        getRightPoke() 
        && ( activeSensor == ActiveSensor::RIGHT || activeSensor == ActiveSensor::BOTH ) 
        && r <= int(chance * 100)
    ) {
        _reward = rightReward;
        return true;
    }

    return false;
}

bool FED4::checkFeedingWindow() {
    if (!feedWindow) return false;
    
    DateTime now = getDateTime();
    
    if ( 
        windowEnd > windowStart
        && now.hour() >= windowStart
        && now.hour() < windowEnd
    ) {
        return true;
    }

    if (
        windowEnd < windowStart
        && ( 
            now.hour() >= windowStart
            || now.hour() < windowEnd
        )
    ) {
        return true;
    }

    return false;
}


// void FED4::setLightCue() {
//     pause_interrupts();

//     digitalWrite(FED4Pins::MTR_EN, HIGH);
//     __delay(2);
//     strip.clear();

//     if (checkFeedingWindow()) {
//         switch (activeSensor) {
//         case ActiveSensor::BOTH:
//             strip.setPixelColor(8, 5, 2, 0, 0);
//             strip.setPixelColor(9, 5, 2, 0, 0);
//             break;
        
//         case ActiveSensor::LEFT:
//             strip.setPixelColor(9, 5, 2, 0, 0);
//             break;

//         case ActiveSensor::RIGHT:
//             strip.setPixelColor(8, 5, 2, 0, 0);
//             break;
//         }
//         strip.show();
//     } 
//     else {
//         strip.show();
//         digitalWrite(FED4Pins::MTR_EN, LOW);
//     }


//     start_interrupts();
// }

int FED4::getViCountDown() {
    int offset = (float)viAvg * viSpread;
    int lowerBound = viAvg - offset;
    int upperBound = viAvg + offset;
    return random(lowerBound, upperBound);
}

int FED4::getBatteryPercentage() {
    pause_interrupts();
    float batteryVoltage = analogRead(FED4Pins::VBAT);
    start_interrupts();

    batteryVoltage *= 2;
    batteryVoltage *= 3.3;
    batteryVoltage /= 1024;

    int batteryPercentage = 0;
    if (batteryVoltage >= 4.2)
    {
        batteryPercentage = 100;
    }
    else if (batteryVoltage >= 4.15)
    {
        batteryPercentage = 95;
    }
    else if (batteryVoltage >= 4.11)
    {
        batteryPercentage = 90;
    }
    else if (batteryVoltage >= 4.08)
    {
        batteryPercentage = 85;
    }
    else if (batteryVoltage >= 4.02)
    {
        batteryPercentage = 80;
    }
    else if (batteryVoltage >= 3.98)
    {
        batteryPercentage = 75;
    }
    else if (batteryVoltage >= 3.95)
    {
        batteryPercentage = 70;
    }
    else if (batteryVoltage >= 3.91)
    {
        batteryPercentage = 65;
    }
    else if (batteryVoltage >= 3.87)
    {
        batteryPercentage = 60;
    }
    else if (batteryVoltage >= 3.85)
    {
        batteryPercentage = 55;
    }
    else if (batteryVoltage >= 3.84)
    {
        batteryPercentage = 50;
    }
    else if (batteryVoltage >= 3.82)
    {
        batteryPercentage = 45;
    }
    else if (batteryVoltage >= 3.8)
    {
        batteryPercentage = 40;
    }
    else if (batteryVoltage >= 3.79)
    {
        batteryPercentage = 35;
    }
    else if (batteryVoltage >= 3.77)
    {
        batteryPercentage = 30;
    }
    else if (batteryVoltage >= 3.75)
    {
        batteryPercentage = 25;
    }
    else if (batteryVoltage >= 3.73)
    {
        batteryPercentage = 20;
    }
    else if (batteryVoltage >= 3.71)
    {
        batteryPercentage = 15;
    }
    else if (batteryVoltage >= 3.69)
    {
        batteryPercentage = 10;
    }
    else if (batteryVoltage >= 3.61)
    {
        batteryPercentage = 5;
    }
    else
    {
        batteryPercentage = 0;
    }

    return batteryPercentage;
}

DateTime FED4::getDateTime() {
    pause_interrupts();

    DateTime now = rtc.now();

    start_interrupts();
    return now;
}

void FED4::flush_to_sd() {
    pause_interrupts();

    if (_log_buffer_pos == 0) {
        return;
    }

    digitalWrite(FED4Pins::CARD_SEL, LOW);
    digitalWrite(FED4Pins::SHRP_CS, HIGH);

    logFile.write(_log_buffer, _log_buffer_pos);
    _log_buffer_pos = 0;

    logFile.truncate();
    char logFileName[30];
    logFile.getName(logFileName, 30);
    logFile.close();
    logFile.open(logFileName, FILE_WRITE);

    start_interrupts();
}

void FED4::write_to_log(char row[ROW_MAX_LEN], bool forceFlush) {
    pause_interrupts();

    int rowLen = strlen(row);
    if(_log_buffer_pos + rowLen >= FILE_RAM_BUFF_SIZE) {
        flush_to_sd();
        _last_flush = millis();
        return;
    }
    memcpy(&_log_buffer[_log_buffer_pos], row, rowLen);
    _log_buffer_pos += rowLen;

    if ( (millis() - _last_flush > 50 * 1000) || forceFlush) {
        flush_to_sd();
    }

    start_interrupts();
}

void FED4::start_interrupts() {
    NVIC_DisableIRQ(EIC_IRQn);
    
    EIC->INTFLAG.reg = (1 << digitalPinToInterrupt(FED4Pins::LFT_POKE));
    EIC->INTFLAG.reg = (1 << digitalPinToInterrupt(FED4Pins::RGT_POKE));
    EIC->INTFLAG.reg = (1 << digitalPinToInterrupt(FED4Pins::WELL));
    rtcZero.attachInterrupt(alarm_ISR);
    __DSB();
    NVIC_EnableIRQ(EIC_IRQn);
}

void FED4::pause_interrupts() {
    NVIC_DisableIRQ(EIC_IRQn);
    rtcZero.detachInterrupt();
}

void FED4::left_poke_handler() {
    _sleep_mode = false;

    if (ignorePokes)
        return;

    if (digitalRead(FED4Pins::LFT_POKE) == LOW)
    {
        long millis_now = millis();
        if (millis_now - _startT_left_poke < 50)
            return;
        _startT_left_poke = millis_now;
        _left_poke_started = true;
    }
    
    else
    {
        if (!_left_poke_started)
            return;
        leftPokeCount++;
        Event event = {
            .time = getDateTime(),
            .message = EventMsg::LEFT
        };
        logEvent(event);
        _left_poke_started = false;
        _left_poke = true;
        _dT_left_poke = 0;
    }
}

void FED4::right_poke_handler() {
    _sleep_mode = false;

    if (ignorePokes)
        return;

    if (digitalRead(FED4Pins::RGT_POKE) == LOW)
    {
        long millis_now = millis();
        if (millis_now - _startT_right_poke < 50)
            return;
        _startT_right_poke = millis_now;
        _right_poke_started = true;
    }
    
    else
    {
        if (!_right_poke_started)
            return;
        rightPokeCount++;
        Event event = {
            .time = getDateTime(),
            .message = EventMsg::RIGHT
        };
        logEvent(event);
        _right_poke_started = false;
        _right_poke = true;
        _dT_right_poke = 0;
    }
}

void FED4::alarm_handler() {
    if (_sleep_mode) {
        if (checkFeedingWindow()) {
            _sleep_mode = false;
        }
        
        updateDisplay(true);
        // setLightCue();
        flush_to_sd();

        pause_interrupts();
        
        uint8_t alarmSeconds = rtcZero.getSeconds() + LP_AWAKE_PERIOD - 1;
        uint8_t alarmMinutes = rtcZero.getMinutes() + (alarmSeconds / 60);
        uint8_t alarmHours = rtcZero.getHours() + (alarmMinutes / 60);
        alarmSeconds = alarmSeconds % 60;
        alarmMinutes = alarmMinutes % 60;
        alarmHours = alarmHours % 24;
        rtcZero.setAlarmTime(alarmHours, alarmMinutes, alarmSeconds);
        
        watch_dog.clear();

        start_interrupts();
    }
}

void FED4::well_handler() {
#if OLD_WELL
    if(digitalRead(FED4Pins::WELL) == LOW) {
        _pellet_in_well = true;
    }
    else {
        _pellet_in_well = false;
    }
#else 
    if (millis() - _last_pellet_t > 100) {
        _pellet_dropped = true;
        _last_pellet_t = millis();
    }
#endif
}

void FED4::left_poke_IRS() {
    if (instance) {
        instance->left_poke_handler();
    }
}

void FED4::right_poke_IRS() {
    if (instance) {
        instance->right_poke_handler();
    }
}

void FED4::well_ISR() {
    if (instance) {
        instance->well_handler();
    }
}

void FED4::alarm_ISR() {
    if (instance) {
        instance->alarm_handler();
    }
}

void FED4::dateTime(uint16_t *date, uint16_t *time) {
    DateTime now = instance->getDateTime();
    *date = FAT_DATE(now.year(), now.month(), now.day());
    *time = FAT_TIME(now.hour(), now.minute(), now.second());
}

void FED4::wtd_shut_down() {
    if(instance) {
        // instance->flush_to_sd();
    }
}

void  FED4::wtd_restart() {
    pause_interrupts();

    watch_dog.setup(_wtd_timeout);

    FatFile root;
    root.open("/", O_READ);
    root.rewind();

    char latestName[30] = "";
    uint16_t latestTime = 0;
    uint16_t latestDate = 0;

    SdFile file;
    while (file.openNext(&root, O_READ)) {
        uint16_t date, time;
        char name[30] = "";
        file.getModifyDateTime(&date, &time);
        file.getName(name, 30);
        if (
            ( date > latestDate 
            || (date == latestDate && time > latestTime) )
            && strncmp(name, "FED", 3) == 0
        ) {
            latestDate = date;
            latestTime = time;
            strncpy(latestName, name, 30);
        }
        file.close();
    }

    if (!logFile.open(latestName, O_RDWR | O_AT_END) || strlen(latestName) == 0) {
        initLogFile();
        Event event = {
            time: getDateTime(),
            message: EventMsg::WTD_RTS
        };
        logEvent(event);
        flush_to_sd();
        start_interrupts();
        return;
    }

    char header[500] = "";

    logFile.seekSet(0);
    logFile.read(header, 500);
    
    uint8_t leftPoke_idx = 0;
    uint8_t rightPoke_idx = 0;
    uint8_t pellets_idx = 0;
    
    char* headerPtr = strtok(header, "\n"); 
    char *column = strtok(headerPtr, ",");
    uint8_t columnIdx = 0;
    while (column != nullptr) {
        if (strcmp(column, "Left Poke Count") == 0) {
            leftPoke_idx = columnIdx;
        }
        else if (strcmp(column, "Right Poke Count") == 0) {
            rightPoke_idx = columnIdx;
        }
        else if (strcmp(column, "Pellet Count") == 0) {
            pellets_idx = columnIdx;
        }
        columnIdx++;
        column = strtok(nullptr, ",");
    }
    
    char lastRow[500] = "";

    logFile.seekEnd(-1);
    char c = logFile.read();
    while (c == '\n') {
        logFile.seekCur(-2);
        c = logFile.read();
    }
    logFile.truncate(logFile.curPosition());
    
    if ((int)logFile.fileSize() - 1000 < 0) {
        logFile.seekSet(0);
    } else {
        logFile.seekEnd(-1000);
    }
    char endRows[1001];
    memset(endRows, 0, 1001);
    
    logFile.read(endRows, 1000);
    int pos = strlen(endRows) - 1;
    while(endRows[pos] != '\n') {
        pos --;
    }
    strncpy(lastRow, endRows + pos + 1, 500);

    uint16_t leftPokes = 0;
    uint16_t rightPokes = 0;
    uint16_t pellets = 0;

    if (strncmp(lastRow, header, strlen(lastRow)) != 0) {
        int8_t idx = 0;
        char *token = strtok(lastRow, ",");
        while (token != nullptr) {
            if (idx == leftPoke_idx) {
                leftPokes = atoi(token);
            }
            else if (idx == rightPoke_idx) {
                rightPokes = atoi(token);
            }
            else if (idx == pellets_idx) {
                pellets = atoi(token);
            }
            idx++;
            token = strtok(nullptr, ",");
        }
    }

    logFile.seekEnd();
    logFile.print("\n");
    
    leftPokeCount = leftPokes;
    rightPokeCount = rightPokes;
    pelletsDispensed = pellets;

    Event event = {
        time: getDateTime(),
        message: EventMsg::WTD_RTS
    };
    logEvent(event);
    flush_to_sd();

    start_interrupts();
}