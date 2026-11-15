#include "RecordEnumerationImpl.h"
#include "RecordStoreException.h"

RecordEnumerationImpl::RecordEnumerationImpl(std::vector<std::vector<int8_t>> data)
{
    this->data = data;
}

RecordEnumerationImpl::RecordEnumerationImpl()
{
}

RecordEnumerationImpl::~RecordEnumerationImpl()
{
    data.clear();
}

int RecordEnumerationImpl::numRecords()
{
    return data.size();
}

std::vector<int8_t> RecordEnumerationImpl::nextRecord()
{
    return data[currentPos++];
}

int RecordEnumerationImpl::addRecord(std::vector<int8_t> bytes)
{
    data.push_back(bytes);
    return data.size() - 1;
}

void RecordEnumerationImpl::setRecord(int index, std::vector<int8_t> bytes)
{
    if (static_cast<int>(data.size()) <= index) {
        throw RecordStoreException();
    }
    data[index] = bytes;
}

void RecordEnumerationImpl::reset()
{
    currentPos = 0;
}

int RecordEnumerationImpl::nextRecordId()
{
    if (currentPos >= static_cast<int>(data.size())) {
        throw RecordStoreException();
    }
    return currentPos;
}

// `destroy()` in J2ME released the *enumeration*; the records stayed in the
// store. Here the store hands out its own record list (RecordStore::
// enumerateRecords() returns this object), so clearing it took the records away
// from the store itself: MenuManager's initPart(2) reads the state, calls this,
// and from then on every setRecord() hits an empty list, throws, and the save
// writes nothing. Which is what "the save does not load" was: the second session
// onwards never wrote a record back.
void RecordEnumerationImpl::destroy()
{
}

void RecordEnumerationImpl::serialize(FileStream* outStream)
{
    size_t temp = data.size();
    outStream->writeVariable(&currentPos);
    outStream->writeVariable(&(temp = data.size()));

    for (auto i = data.cbegin(); i != data.cend(); i++) {
        outStream->writeVariable(&(temp = i->size()));
        for (auto j = i->cbegin(); j != i->cend(); j++) {
            int8_t buffer;
            outStream->writeVariable(&(buffer = *j));
        }
    }
}

void RecordEnumerationImpl::deserialize(FileStream* inStream)
{
    // A store file on this card can be short, zero length, or half written: the
    // card has had FAT errors. Reading a count out of it and resizing to it is an
    // allocation sized by whatever happened to be in those bytes, so each count
    // is checked before it is used and a store that does not parse is simply
    // empty -- the game then starts with no records instead of dying on the
    // allocation.
    size_t temp = 0;
    inStream->readVariable(&currentPos);
    inStream->readVariable(&temp);
    if (!inStream->good() || temp > MAX_RECORDS) {
        data.clear();
        currentPos = 0;
        return;
    }
    data.assign(temp, std::vector<int8_t>());

    for (size_t i = 0; i < data.size(); ++i) {
        inStream->readVariable(&temp);
        if (!inStream->good() || temp > MAX_RECORD_BYTES) {
            data.clear();
            currentPos = 0;
            return;
        }
        data[i].resize(temp);
        for (size_t j = 0; j < data[i].size(); ++j) {
            inStream->readVariable(&data[i][j]);
            if (!inStream->good()) {
                data.clear();
                currentPos = 0;
                return;
            }
        }
    }
}
