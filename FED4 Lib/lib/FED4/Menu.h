#ifndef MENU_H
#define MENU_H

#include <Arduino.h>
#include <RTClib.h>
#include <Adafruit_SharpMem.h>

extern Adafruit_SharpMem *menu_display;
extern RTC_PCF8523 *menu_rtc;

constexpr uint8_t BLACK = 0;
constexpr uint8_t WHITE = 1;

constexpr uint8_t LEFT_POKE   = 6;
constexpr uint8_t RIGHT_POKE  = 5;

constexpr uint8_t COL_1_X     = 10;
constexpr uint8_t COL_2_X     = 100;
constexpr uint8_t START_Y     = 15;
constexpr uint8_t ROW_HEIGHT  = 20;

constexpr int8_t SEL_DONE = -1;
constexpr int8_t SEL_BACK = -2;

typedef enum {
    ITEM_T_INT,
    ITEM_T_FLOAT,
    ITEM_T_BOOL,
    ITEM_T_LIST,
    ITEM_T_SUBMENU
} ItemType;

typedef enum {
    INT32,
    UINT8,
    INT8
} IntType;

typedef enum {
    MENU_T_LIST,
    MENU_T_CLOCK
} MenuType;

typedef struct MenuItem MenuItem;
typedef struct Menu Menu;

typedef struct MenuItem {
    MenuItem(const char* name, uint8_t*value, uint8_t min, uint8_t max, uint8_t step);
    MenuItem(const char* name, int8_t*value, int8_t min, int8_t max, int8_t step);
    MenuItem(const char* name, int* value, int min, int max, int step);
    MenuItem(const char* name, float* value, float min, float max, float step);
    MenuItem(const char* name, bool* value);
    MenuItem(const char *name, int8_t* idx, const char** list, int listLen);
    MenuItem(const char *name, uint8_t* idx, const char** list, int listLen);
    MenuItem(const char *name, Menu *submenu);
    ~MenuItem();

    const char *name;
    ItemType type;
    IntType intType;

    void *value;

    void *minValue;
    void *maxValue;
    void *step;

    uint8_t* valueIdx;
    const char **list;
    int listLen;

    Menu *submenu;
} MenuItem;
const char** initList(int listLen);
void increaseInt(MenuItem *item);
void decreaseInt(MenuItem *item);
void increaseFloat(MenuItem *item);
void decreaseFloat(MenuItem *item);
void changeBool(MenuItem *item);
void nextList(MenuItem *item);
void previousList(MenuItem *item);


typedef struct Menu {
    Menu();
    Menu(int itemNo);
    Menu(Menu *parent, int itemNo);
    ~Menu();

    Menu *parent;
    MenuType type;
    MenuItem **items;
    uint8_t itemNo = 0;
    uint8_t capacity = 0;
    MenuItem *selectedItem;
    int selectedIdx;

    void add(const char* name, uint8_t*value, uint8_t min, uint8_t max, uint8_t step);
    void add(const char* name, int8_t*value, int8_t min, int8_t max, int8_t step);
    void add(const char* name, int* value, int min, int max, int step);
    void add(const char* name, float* value, float min, float max, float step);
    void add(const char* name, bool* value);
    void add(const char *name, uint8_t* idx, const char** list, int listLen);
    void add(const char *name, int8_t* idx, const char** list, int listLen);
    void add(const char *name, Menu *submenu);
    void add(MenuItem* item);
    void run(int batteryLevel = -1);
} Menu;
void handleRightBtn(Menu *item);
void handleLeftBtn(Menu *item);

struct ClockMenu : Menu {
    ClockMenu();
    ClockMenu(Menu *parent);
    ~ClockMenu();

    int *day;
    int *month;
    int *year;
    int *hour;
    int *minute;
};

#endif