#pragma once

#include <cstdint>

namespace daily {

// UE5 布局快照：全部字段由 package 内 profile/compatibility.profile.json 的 layout 填充，
// 代码里不保留第二份数值。加载失败时 layout_ready 保持 false，依赖反射的功能停用
// （界面驱动的领奖链仍然可用）。
struct Layout {
    std::uint32_t object_internal_index{};
    std::uint32_t object_class{};
    std::uint32_t object_name{};
    std::uint32_t ustruct_super{};
    std::uint32_t ustruct_children{};
    std::uint32_t ufield_next{};
    std::uint32_t ufunction_num_parms{};
    std::uint32_t ufunction_parms_size{};
    std::uint32_t process_event_vtable_slot;
    std::uint32_t objects_items{};
    std::uint32_t objects_count{};
    std::uint32_t objects_num_chunks{};
    std::uint32_t objects_chunk_size{};
    std::uint32_t object_item_stride{};
    std::uint32_t objects_base_subtract{};
    std::uint32_t world_game_instance{};
    std::uint32_t game_instance_local_players{};
    std::uint32_t local_player_controller{};
    std::uint32_t controller_player_state{};
    std::uint32_t rpc_message_type_offset{};
    std::uint32_t store_management_receive_type{};
};

}  // namespace daily
