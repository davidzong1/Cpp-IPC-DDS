#pragma once
#include "libipc/buffer.h"

namespace dzIPC {
class DataBase
{
public:
    DataBase() = default;
    ~DataBase() = default;

    virtual bool check_msg_id(const ipc::buffer& data) = 0;

    int msg_method{-1};
};
}   // namespace dzIPC