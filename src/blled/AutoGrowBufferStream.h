// Holds one incoming printer message while it arrives (the MQTT client streams big messages here).
// Originally from XTouch's AutoGrowBufferStream. BLLED Plus rewrite:
// - begin(size) is called with the message size up front, so the buffer is sized once instead of
//   growing 128 bytes at a time (which chopped memory into pieces too small to use)
// - the buffer is kept between messages (they're all about the same size) instead of shrinking
//   and regrowing for each one
// - if there isn't enough memory for a message, that one message is skipped cleanly; it never
//   continues with half a message, never glues a cut-off message onto the next, and never loses
//   its buffer (which used to crash the controller)
#ifndef AutoGrowBufferStream_h
#define AutoGrowBufferStream_h

#include <Arduino.h>
#include <Stream.h>

#define MAX_BUFFER_SIZE 65536
#define KEEP_BUFFER_SIZE 24576 // after a larger message, give the extra memory back

class AutoGrowBufferStream : public Stream
{
private:
    size_t _len = 0;
    size_t _cap = 0;
    char *_buffer = nullptr;
    bool _overflow = false;
    size_t _wanted = 0; // size of the message that didn't fit

    bool reserve(size_t need)
    {
        if (need <= _cap)
            return true;
        if (need > MAX_BUFFER_SIZE + 1)
            return false;
        size_t cap = _cap ? _cap : 1024;
        while (cap < need)
            cap *= 2;
        if (cap > MAX_BUFFER_SIZE + 1)
            cap = need;
        char *tmp = (char *)realloc(_buffer, cap);
        if (!tmp && cap > need)
            tmp = (char *)realloc(_buffer, cap = need); // try exactly what's needed
        if (!tmp)
            return false; // _buffer is still valid
        _buffer = tmp;
        _cap = cap;
        return true;
    }

public:
    AutoGrowBufferStream() { reserve(1024); }
    ~AutoGrowBufferStream() { free(_buffer); }

    // A new message of this many bytes is about to arrive
    void begin(size_t expected)
    {
        _len = 0;
        _overflow = false;
        _wanted = expected;
        if (!reserve(expected + 1))
            _overflow = true;
    }

    virtual size_t write(uint8_t byte)
    {
        if (_overflow)
            return 0;
        if (_len + 2 > _cap && !reserve(_len + 2))
        {
            _overflow = true;
            _wanted = _len + 1;
            return 0;
        }
        _buffer[_len++] = byte;
        return 1;
    }

    virtual int read() { return 0; }
    virtual int available() { return 1; }
    int peek() { return 0; }

    // Done with the message: empty it, keep the memory unless it got unusually big
    virtual void flush()
    {
        _len = 0;
        _overflow = false;
        if (_cap > KEEP_BUFFER_SIZE)
        {
            char *tmp = (char *)realloc(_buffer, 4096);
            if (tmp)
            {
                _buffer = tmp;
                _cap = 4096;
            }
        }
    }

    // The printer connection is paused (update from GitHub): hand the whole buffer back, so the
    // memory isn't split up while the secure connection to GitHub needs large pieces
    void release()
    {
        _len = 0;
        _overflow = false;
        free(_buffer);
        _buffer = (char *)malloc(1024);
        _cap = _buffer ? 1024 : 0;
    }

    bool overflowed() const { return _overflow; }
    size_t wanted() const { return _wanted; }
    size_t current_length() const { return _len; }
    const char *get_buffer() const { return _buffer; }
    const char *get_string() const
    {
        if (_buffer && _len < _cap)
            _buffer[_len] = '\0';
        return _buffer ? _buffer : "";
    }

    using Print::write;
};

#endif
