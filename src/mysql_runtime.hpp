#pragma once
namespace batt {
// Main owns Library; each Worker owns Thread outside its Store lifetime.
class MysqlLibrary {
public:
    MysqlLibrary();
    ~MysqlLibrary();
    MysqlLibrary(const MysqlLibrary&) = delete;
    MysqlLibrary& operator=(const MysqlLibrary&) = delete;
};
class MysqlThread {
public:
    MysqlThread();
    ~MysqlThread();
    MysqlThread(const MysqlThread&) = delete;
    MysqlThread& operator=(const MysqlThread&) = delete;
};
const char* mysql_client_version();
} // namespace batt
