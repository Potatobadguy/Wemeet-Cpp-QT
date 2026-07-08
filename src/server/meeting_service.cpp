#include "meeting_service.h"
#include "logger.h"
#include <random>
#include <sstream>
#include <algorithm>

namespace wemeet {

std::string MeetingService::generate_room_id() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    std::ostringstream oss;
    oss << std::hex << dist(gen) << dist(gen);
    return oss.str().substr(0, 12);
}

MeetingService::CreateResult MeetingService::create_meeting(
        uint64_t host_id, const std::string& title, int max_participants) {

    CreateResult result{false, "", ""};

    std::string room_id = generate_room_id();

    // 持久化到数据库
    auto db_id = meeting_repo_.create(host_id, room_id, title, max_participants);
    if (!db_id.has_value()) {
        result.error = "Database error";
        return result;
    }

    // 创建内存中的房间状态
    auto room = std::make_shared<RoomInfo>();
    room->room_id  = room_id;
    room->title    = title;
    room->host_id  = host_id;
    room->max_participants = max_participants;
    room->created_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    {
        std::unique_lock lock(rooms_mutex_);
        rooms_[room_id] = room;
    }

    result.success = true;
    result.room_id = room_id;

    LOG_INFO("Meeting created: room=%s, host=%lu, title=%s",
             room_id.c_str(), host_id, title.c_str());
    return result;
}

MeetingService::JoinResult MeetingService::join_meeting(
        uint64_t user_id, const std::string& nickname,
        const std::string& room_id) {

    JoinResult result{false, "", {}};

    std::shared_ptr<RoomInfo> room;
    {
        std::shared_lock lock(rooms_mutex_);
        auto it = rooms_.find(room_id);
        if (it == rooms_.end()) {
            result.error = "Room not found";
            return result;
        }
        room = it->second;
    }

    // 检查人数上限
    if (static_cast<int>(room->participants.size()) >= room->max_participants) {
        result.error = "Room is full";
        return result;
    }

    // 检查是否已在房间
    for (auto& p : room->participants) {
        if (p.user_id == user_id) {
            result.error = "Already in room";
            return result;
        }
    }

    // 添加参与者
    RoomParticipant participant;
    participant.user_id  = user_id;
    participant.nickname = nickname;
    participant.is_host  = (user_id == room->host_id);

    room->participants.push_back(participant);

    result.success = true;
    result.participants = room->participants;

    LOG_INFO("User joined meeting: user=%lu, room=%s, total=%zu",
             user_id, room_id.c_str(), room->participants.size());
    return result;
}

void MeetingService::leave_meeting(uint64_t user_id, const std::string& room_id) {
    std::shared_lock lock(rooms_mutex_);
    auto it = rooms_.find(room_id);
    if (it == rooms_.end()) return;

    auto& room = it->second;
    room->participants.erase(
        std::remove_if(room->participants.begin(), room->participants.end(),
                        [user_id](const RoomParticipant& p) {
                            return p.user_id == user_id;
                        }),
        room->participants.end());

    LOG_INFO("User left meeting: user=%lu, room=%s, remaining=%zu",
             user_id, room_id.c_str(), room->participants.size());
}

void MeetingService::end_meeting(const std::string& room_id, uint64_t host_id) {
    {
        std::unique_lock lock(rooms_mutex_);
        auto it = rooms_.find(room_id);
        if (it == rooms_.end()) return;

        auto& room = it->second;
        if (room->host_id != host_id) return;  // 只有主持人可以结束

        rooms_.erase(it);
    }

    meeting_repo_.end_meeting(room_id);
    LOG_INFO("Meeting ended: room=%s, by host=%lu", room_id.c_str(), host_id);
}

std::shared_ptr<RoomInfo> MeetingService::get_room(const std::string& room_id) {
    std::shared_lock lock(rooms_mutex_);
    auto it = rooms_.find(room_id);
    if (it != rooms_.end()) return it->second;
    return nullptr;
}

void MeetingService::broadcast_to_room(
        const std::string& room_id, uint64_t exclude_user,
        const std::function<void(uint64_t)>& sender) {

    auto room = get_room(room_id);
    if (!room) return;

    for (auto& p : room->participants) {
        if (p.user_id != exclude_user) {
            sender(p.user_id);
        }
    }
}

} // namespace wemeet
