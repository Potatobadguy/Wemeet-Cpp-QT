#pragma once
#include "connection_pool.h"
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <chrono>

namespace wemeet {

/**
 * @brief 会议数据仓库
 */
struct MeetingRecord {
    uint64_t id;
    std::string room_id;
    uint64_t creator_id;
    std::string title;
    int64_t  start_time;
    int64_t  end_time;
    int      max_participants;
    std::string record_path;
    int      status;   // 0:进行中 1:已结束
};

class MeetingRepository {
public:
    explicit MeetingRepository(ConnectionPool* pool) : pool_(pool) {}

    // 创建会议记录
    std::optional<uint64_t> create(uint64_t creator_id,
                                    const std::string& room_id,
                                    const std::string& title,
                                    int max_participants = 50);

    // 结束会议
    bool end_meeting(const std::string& room_id);

    // 查找会议（利用 room_id 唯一索引）
    std::optional<MeetingRecord> find_by_room_id(const std::string& room_id);

    // 历史会议分页查询
    std::vector<MeetingRecord> get_user_history(uint64_t user_id,
                                                  int offset = 0, int limit = 20);

private:
    ConnectionPool* pool_;
};

} // namespace wemeet
