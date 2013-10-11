/*
 * Simple ARM debug interface for Arduino, using the SWD (Serial Wire Debug) port.
 * 
 * Copyright (c) 2013 Micah Elizabeth Scott
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <Arduino.h>
#include <stdarg.h>
#include "arm_debug.h"

// Debug port registers
enum DebugPortReg {
    ABORT = 0x0,
    IDCODE = 0x0,
    CTRLSTAT = 0x4,
    SELECT = 0x8,
    RDBUFF = 0xC
};

// CTRL/STAT bits
enum CtrlStatBit {
    CSYSPWRUPACK = 1 << 31,
    CSYSPWRUPREQ = 1 << 30,
    CDBGPWRUPACK = 1 << 29,
    CDBGPWRUPREQ = 1 << 28
};

// Memory Access Port registers
enum MemPortReg {
    MEM_CSW = 0x00,
    MEM_TAR = 0x04,
    MEM_DRW = 0x0C,
    MEM_IDR = 0xFC,
};


bool ARMDebug::begin(unsigned clockPin, unsigned dataPin, LogLevel logLevel)
{
    this->clockPin = clockPin;
    this->dataPin = dataPin;
    this->logLevel = logLevel;
    pinMode(clockPin, OUTPUT);
    pinMode(dataPin, INPUT_PULLUP);

    // Invalidate cache
    cache.select = 0xFFFFFFFF;

    // Put the bus in a known state, and trigger a JTAG-to-SWD transition.
    wireWriteTurnaround();
    wireWrite(0xFFFFFFFF, 32);
    wireWrite(0xFFFFFFFF, 32);
    wireWrite(0xE79E, 16);
    wireWrite(0xFFFFFFFF, 32);
    wireWrite(0xFFFFFFFF, 32);
    wireWrite(0, 32);
    wireWrite(0, 32);

    // Retrieve IDCODE
    uint32_t idcode;
    if (!dpRead(IDCODE, false, idcode)) {
        log(LOG_ERROR, "No ARM processor detected. Check power and cables?");
        return false;
    }
    
    // Verify debug port part number only. This isn't allowed to change, and it's a good early sanity check.
    if ((idcode & 0x0FF00001) != 0x0ba00001) {
        // For reference, the K20's IDCODE is 0x4ba00477 over JTAG vs. 0x2ba01477 over SWD.
        log(LOG_ERROR, "ARM Debug Port has an incorrect part number (IDCODE: %08x)", idcode);
        return false;
    }
    log(LOG_NORMAL, "Found ARM processor debug port (IDCODE: %08x)", idcode);

    // Initialize CTRL/STAT, request system and debugger power-up
    if (!dpWrite(CTRLSTAT, false, CSYSPWRUPREQ | CDBGPWRUPREQ))
        return false;

    // Wait for power-up acknowledgment
    uint32_t powerAck = CDBGPWRUPACK | CSYSPWRUPACK;
    unsigned retries = 4;
    uint32_t ctrlstat;
    while (retries--) {
        if (!dpRead(CTRLSTAT, false, ctrlstat))
            return false;
        if ((ctrlstat & powerAck) == powerAck)
            break;
    }
    if (!retries) {
        log(LOG_ERROR, "ARMDebug: Debug port failed to power on (CTRLSTAT: %08x)", ctrlstat);
        return false;
    }

    // Make sure the default debug access port is an AHB (memory bus) port, like we expect
    uint32_t idr;
    if (!apRead(0, MEM_IDR, idr))
        return false;
    if ((idr & 0xF) != 1) {
        log(LOG_ERROR, "ARMDebug: Default access port is not an AHB-AP as expected! (IDR: %08x)", idr);
        return false;
    }

    // Set up default CSW values for the AHB-AP. Use 32-bit accesses with auto-increment.
    uint32_t csw = (1 << 6) |  // Device enable
                   (1 << 4) |  // Increment by a single word
                   (2 << 0) ;  // 32-bit data size
    if (!apWrite(0, MEM_CSW, csw))
        return false;

    return true;
}

bool ARMDebug::memStore(uint32_t addr, uint32_t data)
{
    return memStore(addr, &data, 1);
}

bool ARMDebug::memLoad(uint32_t addr, uint32_t &data)
{
    return memLoad(addr, &data, 1);
}

bool ARMDebug::memStore(uint32_t addr, uint32_t *data, unsigned count)
{
    if (!apWrite(0, MEM_TAR, addr))
        return false;

    while (count) {
        log(LOG_TRACE, "MEM Store [%08x] %08x", addr, *data);

        if (!apWrite(0, MEM_DRW, *data))
            return false;

        data++;
        addr++;
        count--;
    }

    return true;
}

bool ARMDebug::memLoad(uint32_t addr, uint32_t *data, unsigned count)
{
    if (!apWrite(0, MEM_TAR, addr))
        return false;

    while (count) {
        if (!apRead(0, MEM_DRW, *data))
            return false;

        log(LOG_TRACE, "MEM Load  [%08x] %08x", addr, *data);

        data++;
        addr++;
        count--;
    }

    return true;
}

bool ARMDebug::apWrite(unsigned accessPort, unsigned addr, uint32_t data)
{
    return dpSelect(accessPort, addr) && dpWrite(addr, true, data);
}

bool ARMDebug::apRead(unsigned accessPort, unsigned addr, uint32_t &data)
{
    return dpSelect(accessPort, addr) && dpRead(addr, true, data);
}

bool ARMDebug::dpSelect(unsigned accessPort, unsigned addr)
{
    /*
     * Select a new access port and/or a bank (high nybble of AP address).
     * This is cached, so redundant dpSelect()s have no effect.
     */

    uint32_t select = (accessPort << 24) | (addr & 0xF0);
    if (select != cache.select) {
        if (!dpWrite(SELECT, false, select))
            return false;
        cache.select = select;
    }
    return true;
}

bool ARMDebug::dpWrite(unsigned addr, bool APnDP, uint32_t data)
{
    unsigned retries = 10;
    unsigned ack;
    log(LOG_TRACE, "DP  Write [%x:%x] %08x", addr, APnDP, data);

    do {
        wireWrite(packHeader(addr, APnDP, false), 8);
        wireReadTurnaround();
        ack = wireRead(3);
        wireWriteTurnaround();

        switch (ack) {
            case 1:     // Success
                wireWrite(data, 32);
                wireWrite(evenParity(data), 1);
                wireWrite(0, 8);
                return true;

            case 2:     // WAIT
                wireWrite(0, 8);
                retries--;
                break;

            case 4:     // FAULT
                wireWrite(0, 8);
                log(LOG_ERROR, "FAULT response during write (addr=%x APnDP=%d data=%08x)",
                    addr, APnDP, data);
                return false;

            default:
                wireWrite(0, 8);
                log(LOG_ERROR, "PROTOCOL ERROR response during write (ack=%x addr=%x APnDP=%d data=%08x)",
                    ack, addr, APnDP, data);
                return false;
        }
    } while (retries--);

    log(LOG_ERROR, "WAIT timeout during write (addr=%x APnDP=%d data=%08x)",
        addr, APnDP, data);
    return false;
}

bool ARMDebug::dpRead(unsigned addr, bool APnDP, uint32_t &data)
{
    unsigned retries = 10;
    unsigned ack;

    do {
        wireWrite(packHeader(addr, APnDP, true), 8);
        wireReadTurnaround();
        ack = wireRead(3);

        switch (ack) {
            case 1:     // Success
                data = wireRead(32);
                if (wireRead(1) != evenParity(data)) {
                    wireWriteTurnaround();
                    wireWrite(0, 8);
                    log(LOG_ERROR, "PARITY ERROR during read (addr=%x APnDP=%d data=%08x)",
                        addr, APnDP, data);         
                    return false;
                }
                wireWriteTurnaround();
                wireWrite(0, 8);
                log(LOG_TRACE, "DP  Read  [%x:%x] %08x", addr, APnDP, data);
                return true;

            case 2:     // WAIT
                wireWriteTurnaround();
                wireWrite(0, 8);
                retries--;
                break;

            case 4:     // FAULT
                wireWriteTurnaround();
                wireWrite(0, 8);
                log(LOG_ERROR, "FAULT response during read (addr=%x APnDP=%d)", addr, APnDP);
                return false;

            default:
                wireWriteTurnaround();
                wireWrite(0, 8);
                log(LOG_ERROR, "PROTOCOL ERROR response during read (ack=%x addr=%x APnDP=%d)", ack, addr, APnDP);
                return false;
        }
    } while (retries--);

    log(LOG_ERROR, "WAIT timeout during read (addr=%x APnDP=%d)", addr, APnDP);
    return false;
}

uint8_t ARMDebug::packHeader(unsigned addr, bool APnDP, bool RnW)
{
    bool a2 = (addr >> 2) & 1;
    bool a3 = (addr >> 3) & 1;
    bool parity = APnDP ^ RnW ^ a2 ^ a3;
    return (1 << 0)              |  // Start
           (APnDP << 1)          |
           (RnW << 2)            |
           ((addr & 0xC) << 1)   |
           (parity << 5)         |
           (1 << 7)              ;  // Park
}

bool ARMDebug::evenParity(uint32_t word)
{
    word = (word & 0xFFFF) ^ (word >> 16);
    word = (word & 0xFF) ^ (word >> 8);
    word = (word & 0xF) ^ (word >> 4);
    word = (word & 0x3) ^ (word >> 2);
    word = (word & 0x1) ^ (word >> 1);
    return word;
}

void ARMDebug::wireWrite(uint32_t data, unsigned nBits)
{
    log(LOG_TRACE, "SWD Write %08x (%d)", data, nBits);

    while (nBits--) {
        digitalWrite(dataPin, data & 1);
        data >>= 1;
        digitalWrite(clockPin, LOW);
        digitalWrite(clockPin, HIGH);
    }
}

uint32_t ARMDebug::wireRead(unsigned nBits)
{
    uint32_t result = 0;
    uint32_t mask = 1;
    unsigned count = nBits;

    while (count--) {
        if (digitalRead(dataPin)) {
            result |= mask;
        }
        mask <<= 1;
        digitalWrite(clockPin, LOW);
        digitalWrite(clockPin, HIGH);
    }

    log(LOG_TRACE, "SWD Read  %08x (%d)", result, nBits);
    return result;
}

void ARMDebug::wireWriteTurnaround()
{
    log(LOG_TRACE, "SWD Write trn");

    digitalWrite(dataPin, HIGH);
    pinMode(dataPin, INPUT_PULLUP);
    digitalWrite(clockPin, LOW);
    digitalWrite(clockPin, HIGH);
    pinMode(dataPin, OUTPUT);
}

void ARMDebug::wireReadTurnaround()
{
    log(LOG_TRACE, "SWD Read  trn");

    digitalWrite(dataPin, HIGH);
    pinMode(dataPin, INPUT_PULLUP);
    digitalWrite(clockPin, LOW);
    digitalWrite(clockPin, HIGH);
}

void ARMDebug::log(int level, const char *fmt, ...)
{
    if (level <= logLevel && Serial) {
        va_list ap;
        char buffer[256];

        va_start(ap, fmt);
        int ret = vsnprintf(buffer, sizeof buffer, fmt, ap);
        va_end(ap);

        Serial.println(buffer);
    }
}