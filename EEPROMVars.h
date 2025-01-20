#include <ESP_EEPROM.h>

// A universal proxy class for data storage in EEPROM
template <typename T>
class EEPROMVar {
    int _address;
    EEPROMClass& _eeprom;

public:
    EEPROMVar(int address, EEPROMClass& eeprom
#if !defined(NO_GLOBAL_INSTANCES) && !defined(NO_GLOBAL_EEPROM)
        = EEPROM
#endif
        )
        : _address(address)
        , _eeprom(eeprom)
    {
    }


    EEPROMVar& operator=(const T& value)
    {
        _eeprom.put(_address, value);
        return *this;
    }

    operator T() const
    {
        T value;
        _eeprom.get(_address, value);
        return value;
    }
};