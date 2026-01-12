#include <Arduino.h>
#include <DisplayMgr.h>
#include <StorageMgr.h>

DisplayMgr displayMgr;
StorageMgr storageMgr;

void setup() {
  Serial.begin(115200);
  Serial.println("Setup started");

  displayMgr.Init();
  displayMgr.DrawTestScreen();

  storageMgr.Init();

  Serial.println("Setup completed");
}

void loop() {

}