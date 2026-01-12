#include <StorageMgr.h>

StorageMgr::StorageMgr()
{
    Serial.println("====StorageMgr");
}

StorageMgr::~StorageMgr()
{
    Serial.println("~~~~StorageMgr");
}

void StorageMgr::Init()
{
    if(!SD.begin())
    {
        Serial.println("[StorageMgr] SD Card Mount Failed");
    }
    else
    {
        Serial.println("[StorageMgr] SD Card Mounted Successfully");
    }
}

