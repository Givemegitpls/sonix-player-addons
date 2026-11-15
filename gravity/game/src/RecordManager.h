#pragma once

#include <stdint.h>
#include <vector>
#include <string>

class RecordStore;

class RecordManager {
public:
    enum {
        LEAGUES_MAX = 4,
        RECORD_NO_MAX = 3,
        PLAYER_NAME_MAX = 3,
    };

    inline static const int unused = 3;

    void method_8(int var1, int var2);
    std::vector<std::string> getRecordDescription(int var1);
    void writeRecordInfo();
    int getPosOfNewRecord(int league, int64_t timeMs);
    void method_17(int league, char* values, int64_t timeMs);
    void deleteRecordStores();
    void closeRecordStore();

private:
    std::vector<std::vector<int64_t>> recordTimeMs = std::vector<std::vector<int64_t>>(4, std::vector<int64_t>(3));
    // 4: league, 100, 175, 225, 350,
    // 3: three best times
    //
    // One name is PLAYER_NAME_MAX characters and a terminator: the strings are
    // read with std::string(char*) in getRecordDescription(), so the byte after
    // the last one has to be a zero. Zeroed here, because loadRecordInfo() fills
    // the three and upstream left the fourth to whatever the object happened to
    // sit on.
    char recordName[LEAGUES_MAX][RECORD_NO_MAX][PLAYER_NAME_MAX + 1] = {};
    RecordStore* recordStore = nullptr;
    int packedRecordInfoRecordId = -1;
    std::vector<int8_t> packedRecordInfo = std::vector<int8_t>(96);
    std::string str;

    // By reference, all three: in Java these were arrays, which are references,
    // and the decompiled signature lost that. With the buffer passed by value
    // getLevelInfo() packed the times into a copy that was then thrown away, so
    // every record store file was written as 96 zero bytes.
    int64_t load5BytesAsLong(const std::vector<int8_t> &var1, int offset);
    void pushLongAs5Bytes(std::vector<int8_t> &var1, int var2, int64_t var3);
    void loadRecordInfo(const std::vector<int8_t> &var1);
    void getLevelInfo(std::vector<int8_t> &var1);
    void resetRecordsTime();
    void addNewRecord(int gameLevel, int position);
};
