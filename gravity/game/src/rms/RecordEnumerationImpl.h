#pragma once

#include <vector>
#include <iostream>

#include "RecordEnumeration.h"
#include "../utils/FileStream.h"

class RecordEnumerationImpl : public RecordEnumeration {
private:
    // static const int64_t serialVersionUID = 123;
    int currentPos = 0;

    // Ceilings for what a store file may claim. One record is 19 bytes of state
    // or 96 of times; a store holds one of those per level, at most a handful.
    // They exist to keep a corrupt file from turning into a huge allocation.
    static const size_t MAX_RECORDS = 512;
    static const size_t MAX_RECORD_BYTES = 4096;

public:
    RecordEnumerationImpl(std::vector<std::vector<int8_t>> data);
    RecordEnumerationImpl();
    ~RecordEnumerationImpl();

    int numRecords();
    std::vector<int8_t> nextRecord();
    int addRecord(std::vector<int8_t> bytes);
    void setRecord(int index, std::vector<int8_t> bytes);
    void reset();
    int nextRecordId();
    void destroy();

    void serialize(FileStream* outStream);
    void deserialize(FileStream* inStream);

    std::vector<std::vector<int8_t>> data;
};
