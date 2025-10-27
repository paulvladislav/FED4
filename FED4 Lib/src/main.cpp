#include <FED4.h>

FED4 fed4 = FED4();

void setup() {
    fed4.begin();
}


void loop() {
    fed4.run();
}