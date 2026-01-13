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

void StorageMgr::ScanDirectory(const char* path, uint8_t depth)
{
    File dir = SD.open(path);
    if(!dir || !dir.isDirectory()) return;

    File entry;
    while(true)
    {
        entry = dir.openNextFile();
        if(!entry) break;

        FileNode node;
        node.fileName = String(entry.name());
        node.isDirectory = entry.isDirectory();
        node.depth = depth;

        this->fileList.push_back(node);
        if (entry.isDirectory())
        {
            Serial.printf("[StorageMgr] Dir: %s Depth: %d\n", entry.name(), depth);

            // Skip Windows metadata folder, otherwise recurse into it
            if (String(entry.name()) != String("System Volume Information"))
            {
                // Build child path (path may not end with '/')
                String childPath = String(path);
                if (!childPath.endsWith("/")) childPath += "/";
                childPath += entry.name();

                this->ScanDirectory(childPath.c_str(), depth + 1);
            }
            else
            {
                Serial.println("[StorageMgr] Skipping \"System Volume Information\"");
            }
        }
        else
        {
            Serial.printf("[StorageMgr] File: %s Depth: %d Size: %llu bytes\n", entry.name(), depth, entry.size());
        }
        entry.close();
    }
    dir.close();
}

