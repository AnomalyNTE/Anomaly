    struct ReflectedPropertyInfo {
        std::uintptr_t property{};
        std::uintptr_t next{};
        std::string name;
        std::string type;
        std::int32_t array_dim{};
        std::int32_t element_size{};
        std::int32_t offset{};
        std::uintptr_t structure{};
    };
    struct SparseMapView {
        std::uintptr_t map{};
        std::uintptr_t data{};
        std::uintptr_t flags_data{};
        std::int32_t num{};
        std::int32_t num_free{};
        std::int32_t max{};
        std::int32_t flags_num{};
        std::int32_t flags_max{};
        std::int64_t stride{};
        std::int64_t row_offset{};
        std::vector<std::uint32_t> flags;
    };
    struct ReflectedBoolParameter {
        std::uint16_t byte_offset{};
        std::uint8_t field_mask{};
        std::uint8_t byte_mask{};
    };
    struct NativeArrayHeader {
        std::uintptr_t data{};
        std::int32_t count{};
        std::int32_t capacity{};
    };
    struct TeleportBinding {
        std::uintptr_t function{};
        std::uint16_t parms_size{};
        std::uint16_t new_location_offset{};
        std::uint16_t sweep_hit_result_offset{};
        std::uint16_t sweep_hit_result_size{};
        ReflectedBoolParameter b_sweep{};
        ReflectedBoolParameter b_teleport{};
        ReflectedBoolParameter return_value{};
        std::uint64_t object_generation{};
        std::uint32_t next_object_index{};
        bool available{};
        bool discovery_complete{};
    };
    [[nodiscard]] bool ReadReflectedPropertyLocked(
        const std::uintptr_t property,
        ReflectedPropertyInfo& info) const {
        std::uintptr_t name_address{};
        std::uintptr_t next_address{};
        std::uint32_t name_id{};
        ReflectedPropertyInfo candidate;
        candidate.property = property;
        if (property == 0 ||
            !AddAddress(property, layout.ffield_name, name_address) ||
            !AddAddress(
                property, layout.fproperty_propertyLinkNext, next_address) ||
            !ReadValue(*memory, name_address, name_id) ||
            !ReadValue(
                *memory, property + layout.fproperty_arrayDim,
                candidate.array_dim) ||
            !ReadValue(
                *memory, property + layout.fproperty_elementSize,
                candidate.element_size) ||
            !ReadValue(
                *memory, property + layout.fproperty_offsetInternal,
                candidate.offset) ||
            !ReadValue(*memory, next_address, candidate.next) ||
            !(candidate.name = ResolveNameSnapshotLocked(name_id)).size() ||
            !ReadReflectedFieldClassNameLocked(property, candidate.type)) {
            return false;
        }
        if (candidate.type == "StructProperty" &&
            !ReadPointerAt(
                *memory, property, layout.fstructProperty_struct,
                candidate.structure)) {
            return false;
        }
        info = std::move(candidate);
        return true;
    }

    [[nodiscard]] bool ReadReflectedBoolParameterLocked(
        const ReflectedPropertyInfo& property,
        const std::uint16_t parms_size,
        ReflectedBoolParameter& parameter) const noexcept {
        std::uint8_t field_size{};
        std::uint8_t byte_offset{};
        std::uint8_t byte_mask{};
        std::uint8_t field_mask{};
        if (property.type != "BoolProperty" || property.array_dim != 1 ||
            property.element_size != 1 || property.offset < 0 ||
            !ReadValue(
                *memory,
                property.property + layout.fboolProperty_fieldSize,
                field_size) ||
            !ReadValue(
                *memory,
                property.property + layout.fboolProperty_byteOffset,
                byte_offset) ||
            !ReadValue(
                *memory,
                property.property + layout.fboolProperty_byteMask,
                byte_mask) ||
            !ReadValue(
                *memory,
                property.property + layout.fboolProperty_fieldMask,
                field_mask) ||
            field_size == 0 || byte_offset >= field_size || byte_mask == 0 ||
            field_mask == 0 || (byte_mask & field_mask) != byte_mask ||
            static_cast<std::uint64_t>(property.offset) + byte_offset >= parms_size ||
            static_cast<std::uint64_t>(property.offset) + byte_offset >
                (std::numeric_limits<std::uint16_t>::max)()) {
            return false;
        }
        parameter = {
            static_cast<std::uint16_t>(
                static_cast<std::uint32_t>(property.offset) + byte_offset),
            field_mask,
            byte_mask};
        return true;
    }

    [[nodiscard]] bool IsClassDerivedFromLocked(
        std::uintptr_t candidate,
        const std::uintptr_t expected_base) const noexcept {
        constexpr std::size_t kMaximumDepth = 128;
        for (std::size_t depth{};
             candidate != 0 && depth < kMaximumDepth;
             ++depth) {
            if (candidate == expected_base) return true;
            std::uintptr_t next{};
            if (!ReadValue(
                    *memory,
                    candidate + layout.ustruct_superStruct,
                    next) || next == candidate) {
                return false;
            }
            candidate = next;
        }
        return false;
    }

    [[nodiscard]] bool ObjectClassChainContainsLocked(
        const std::uintptr_t object,
        const std::string_view expected) const noexcept {
        std::uintptr_t current{};
        if (!ReadPointerAt(*memory, object, layout.object_class, current)) {
            return false;
        }
        for (std::size_t depth{}; current != 0 && depth < 32; ++depth) {
            std::string name;
            if (!ReadReflectedObjectNameLocked(current, name)) return false;
            if (name == expected) return true;
            std::uintptr_t next{};
            if (!ReadNullablePointerAt(
                    *memory, current, layout.ustruct_superStruct, next) ||
                next == current) {
                return false;
            }
            current = next;
        }
        return false;
    }

    [[nodiscard]] bool ReadSparseMapViewLocked(
        const std::uintptr_t map,
        SparseMapView& view) const noexcept {
        const auto row_map_offset = layout.dataTable_rowMap;
        const auto data_offset = layout.dataTable_rowMapData;
        const auto num_offset = layout.dataTable_rowMapNum;
        const auto num_free_offset = layout.dataTable_rowMapNumFree;
        const auto max_offset = layout.dataTable_rowMapMax;
        const auto stride = layout.dataTable_rowMapElementStride;
        const auto row_offset = layout.dataTable_rowMapRowOffset;
        const auto flags_data_offset = layout.dataTable_rowMapFlagsData;
        const auto flags_num_offset = layout.dataTable_rowMapFlagsNum;
        const auto flags_max_offset = layout.dataTable_rowMapFlagsMax;
        const auto inline_flags_offset = layout.dataTable_rowMapInlineFlags;
        if (map == 0 || row_map_offset < 0 || data_offset < 0 || num_offset < 0 ||
            num_free_offset < 0 ||
            max_offset < 0 || flags_data_offset < 0 || flags_num_offset < 0 ||
            flags_max_offset < 0 || inline_flags_offset < 0 || stride < 16 ||
            stride > 128 || row_offset < 0 ||
            row_offset + static_cast<std::int64_t>(sizeof(std::uintptr_t)) > stride) {
            return false;
        }
        SparseMapView next;
        next.map = map;
        next.stride = stride;
        next.row_offset = row_offset;
        if (!ReadValue(*memory, map + static_cast<std::uintptr_t>(data_offset), next.data) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(num_offset), next.num) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(num_free_offset), next.num_free) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(max_offset), next.max) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(flags_data_offset), next.flags_data) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(flags_num_offset), next.flags_num) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(flags_max_offset), next.flags_max) ||
            next.num < 0 || next.num_free < 0 || next.num_free > next.num ||
            next.num > next.max || next.max > 4096 || next.flags_num < next.num ||
            next.flags_max < next.flags_num || next.flags_num > 4096 ||
            (next.num != 0 && next.data == 0)) {
            return false;
        }
        if (next.num == 0) {
            view = std::move(next);
            return true;
        }
        const auto word_count = static_cast<std::size_t>((next.flags_num + 31) / 32);
        if (word_count == 0 || word_count > 128) return false;
        next.flags.resize(word_count);
        if (next.flags_data != 0) {
            if (!memory->Read(next.flags_data, next.flags.data(),
                              next.flags.size() * sizeof(std::uint32_t))) {
                return false;
            }
        } else if (word_count > 4 || !memory->Read(
                       map + static_cast<std::uintptr_t>(inline_flags_offset),
                       next.flags.data(), next.flags.size() * sizeof(std::uint32_t))) {
            return false;
        }
        view = std::move(next);
        return true;
    }

    [[nodiscard]] bool ReadNativeArrayHeaderLocked(
        const std::uintptr_t address,
        NativeArrayHeader& header,
        const std::int32_t maximum) const noexcept {
        NativeArrayHeader next;
        if (address == 0 || maximum <= 0 ||
            !ReadValue(
                *memory, address + layout.tarray_data, next.data) ||
            !ReadValue(
                *memory, address + layout.tarray_num, next.count) ||
            !ReadValue(
                *memory, address + layout.tarray_max, next.capacity) ||
            next.count < 0 || next.capacity < next.count || next.capacity > maximum ||
            (next.count != 0 && next.data == 0)) {
            return false;
        }
        header = next;
        return true;
    }

    [[nodiscard]] bool BuildTeleportBindingLocked(
        const std::uintptr_t function,
        TeleportBinding& binding) const {
        try {
            std::string function_name;
            if (!ReadReflectedObjectNameLocked(function, function_name) ||
                function_name != "K2_SetActorLocation") {
                return false;
            }

            std::uintptr_t class_object{};
            std::uintptr_t outer_object{};
            if (!ReadPointerAt(*memory, function, layout.object_class, class_object) ||
                !ReadPointerAt(*memory, function, layout.object_outer, outer_object)) {
                return false;
            }
            std::string class_name;
            std::string outer_name;
            if (!ReadReflectedObjectNameLocked(class_object, class_name) ||
                !ReadReflectedObjectNameLocked(outer_object, outer_name) ||
                class_name != "Function" || outer_name != "Actor") {
                return false;
            }

            std::uintptr_t property{};
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_value_offset{};
            if (!ReadPointerAt(
                    *memory, function, layout.ustruct_propertyLink, property) ||
                !ReadValue(*memory, function + layout.ufunction_numParms, num_parms) ||
                !ReadValue(*memory, function + layout.ufunction_parmsSize, parms_size) ||
                !ReadValue(
                    *memory,
                    function + layout.ufunction_returnValueOffset,
                    return_value_offset) ||
                num_parms != 5 || parms_size == 0 || parms_size > 4096) {
                return false;
            }

            TeleportBinding candidate;
            candidate.function = function;
            candidate.parms_size = parms_size;
            candidate.object_generation = object_generation;
            bool found_new_location{};
            bool found_sweep_hit_result{};
            bool found_b_sweep{};
            bool found_b_teleport{};
            bool found_return_value{};
            std::uint32_t property_count{};

            const auto read_bool = [&](const std::int32_t offset, const std::int32_t element_size,
                                       const std::int32_t array_dim,
                                       ReflectedBoolParameter& result) {
                std::uint8_t field_size{};
                std::uint8_t byte_offset{};
                std::uint8_t byte_mask{};
                std::uint8_t field_mask{};
                return array_dim == 1 && element_size == 1 && offset >= 0 &&
                    ReadValue(
                        *memory,
                        property + layout.fboolProperty_fieldSize, field_size) &&
                    ReadValue(
                        *memory,
                        property + layout.fboolProperty_byteOffset, byte_offset) &&
                    ReadValue(
                        *memory,
                        property + layout.fboolProperty_byteMask, byte_mask) &&
                    ReadValue(
                        *memory,
                        property + layout.fboolProperty_fieldMask, field_mask) &&
                    field_size != 0 && byte_offset < field_size && byte_mask != 0 &&
                    field_mask != 0 && (byte_mask & field_mask) == byte_mask &&
                    static_cast<std::uint64_t>(offset) + byte_offset < parms_size &&
                    ((result = {static_cast<std::uint16_t>(
                                      static_cast<std::uint32_t>(offset) + byte_offset),
                                  field_mask,
                                  byte_mask}),
                     true);
            };

            while (property != 0 && property_count < 16) {
                std::uintptr_t next{};
                std::uintptr_t property_name_address{};
                std::uint32_t property_name_id{};
                std::int32_t array_dim{};
                std::int32_t element_size{};
                std::int32_t offset{};
                std::uintptr_t next_address{};
                if (!AddAddress(
                        property, layout.ffield_name, property_name_address) ||
                    !AddAddress(
                        property, layout.fproperty_propertyLinkNext, next_address) ||
                    !ReadValue(*memory, property_name_address, property_name_id) ||
                    !ReadValue(
                        *memory, property + layout.fproperty_arrayDim, array_dim) ||
                    !ReadValue(
                        *memory, property + layout.fproperty_elementSize, element_size) ||
                    !ReadValue(
                        *memory, property + layout.fproperty_offsetInternal, offset) ||
                    !ReadValue(*memory, next_address, next)) {
                    return false;
                }
                const std::string property_name = ResolveNameSnapshotLocked(property_name_id);
                if (property_name.empty() || array_dim != 1 || element_size <= 0 || offset < 0 ||
                    static_cast<std::uint64_t>(offset) +
                            static_cast<std::uint64_t>(element_size) >
                        parms_size) {
                    return false;
                }

                if (property_name == "NewLocation" && !found_new_location) {
                    std::uintptr_t structure{};
                    std::string structure_name;
                    if (element_size != static_cast<std::int32_t>(sizeof(double) * 3U) ||
                        !ReadPointerAt(
                            *memory,
                            property,
                            layout.fstructProperty_struct,
                            structure) ||
                        !ReadReflectedObjectNameLocked(structure, structure_name) ||
                        structure_name != "Vector") {
                        return false;
                    }
                    candidate.new_location_offset = static_cast<std::uint16_t>(offset);
                    found_new_location = true;
                } else if (property_name == "SweepHitResult" && !found_sweep_hit_result) {
                    std::uintptr_t structure{};
                    std::string structure_name;
                    if (!ReadPointerAt(
                            *memory,
                            property,
                            layout.fstructProperty_struct,
                            structure) ||
                        !ReadReflectedObjectNameLocked(structure, structure_name) ||
                        structure_name != "HitResult") {
                        return false;
                    }
                    candidate.sweep_hit_result_offset = static_cast<std::uint16_t>(offset);
                    candidate.sweep_hit_result_size = static_cast<std::uint16_t>(element_size);
                    found_sweep_hit_result = true;
                } else if (property_name == "bSweep" && !found_b_sweep) {
                    if (!read_bool(offset, element_size, array_dim, candidate.b_sweep)) return false;
                    found_b_sweep = true;
                } else if (property_name == "bTeleport" && !found_b_teleport) {
                    if (!read_bool(offset, element_size, array_dim, candidate.b_teleport)) return false;
                    found_b_teleport = true;
                } else if (property_name == "ReturnValue" && !found_return_value) {
                    if (static_cast<std::uint16_t>(offset) != return_value_offset ||
                        !read_bool(offset, element_size, array_dim, candidate.return_value)) {
                        return false;
                    }
                    found_return_value = true;
                } else {
                    return false;
                }

                property = next;
                ++property_count;
            }

            if (property != 0 || property_count != 5 || !found_new_location ||
                !found_sweep_hit_result || !found_b_sweep || !found_b_teleport ||
                !found_return_value) {
                return false;
            }
            candidate.available = true;
            candidate.discovery_complete = true;
            binding = candidate;
            return true;
        } catch (...) {
            return false;
        }
    }
