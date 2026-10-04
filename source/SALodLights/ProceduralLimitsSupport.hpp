#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <Zydis.h>

namespace ProceduralLimits
{
    struct Settings
    {
        int objectInstances = 512;
        int terrainTriangles = 256;
        int collisionEntities = 40;
        int temporaryObjects = 150;
        int matrices = 200;

        void Normalize()
        {
            objectInstances = std::clamp(objectInstances, 512, 65535);
            terrainTriangles = std::clamp(terrainTriangles, 256, 65535);
            collisionEntities = std::clamp(collisionEntities, 40, 65535);
            // CObject::nNoTempObjects is uint16_t.
            temporaryObjects = std::clamp(temporaryObjects, 150, 65535);
            matrices = std::clamp(matrices, 200, 65535);
        }
    };

    // SA's lists are intrusive: backing storage must never move while linked.
    template<typename T>
    void Prepend(T*& head, T& item)
    {
        item.prev = nullptr;
        item.next = head;
        if (head)
            head->prev = &item;
        head = &item;
    }

    template<typename T>
    struct CountedList
    {
        T* head = nullptr;
        T* tail = nullptr;
        uint32_t count = 0;

        void Add(T& item)
        {
            if (!head)
                tail = &item;
            Prepend(head, item);
            ++count;
        }
    };

    template<typename T>
    void AttachUnused(T*& head, std::span<T> entries)
    {
        // Only after the original initializer has rebuilt the free lists and all
        // previous active entries have been released.
        for (auto& entry : entries)
        {
            entry = {};
            Prepend(head, entry);
        }
    }

    template<typename T>
    void AttachUnused(CountedList<T>& list, std::span<T> entries)
    {
        for (auto& entry : entries)
        {
            entry = {};
            list.Add(entry);
        }
    }

    struct Vector
    {
        float x, y, z;
    };

    struct PlantTriangle
    {
        Vector vertices[3];
        Vector center;
        float sphereRadius;
        float seeds[3];
        uint16_t numPlants[3];
        uint8_t surfaceId;
        uint8_t lighting;
        uint8_t flags;
        PlantTriangle* next;
        PlantTriangle* prev;
    };

    struct CollisionEntry
    {
        void* entity;
        PlantTriangle** triangles;
        uint16_t numTriangles;
        CollisionEntry* next;
        CollisionEntry* prev;
    };

    struct ObjectEntry
    {
        ObjectEntry* prev;
        ObjectEntry* next;
        void* object;
        PlantTriangle* triangle;
        bool allocatedMatrix;
    };

    struct SurfaceInfo
    {
        uint8_t properties[0x3C];
        CountedList<ObjectEntry> objects;
    };

    struct ObjectManager
    {
        int32_t allocatedMatrices;
        int32_t numSurfaceInfos;
        SurfaceInfo surfaceInfos[128];
        ObjectEntry objects[512];
        CountedList<ObjectEntry> unused;
    };

    struct AllocationSnapshot
    {
        ObjectEntry* head;
        ObjectEntry* next;
        uint32_t count;

        explicit AllocationSnapshot(const CountedList<ObjectEntry>& list)
            : head(list.head), next(head ? head->next : nullptr), count(list.count)
        {
        }

        bool RecoverFailure(CountedList<ObjectEntry>& list, const SurfaceInfo& surface) const
        {
            // The original AddObject can take a node then reject the allocation.
            // Recover only an otherwise unused node removed by exactly one pop;
            // do not double-insert if another mod already returns it on failure.
            if (!head || !count || list.count != count - 1 || list.head != next ||
                head->object || surface.objects.head == head)
                return false;

            *head = {};
            list.Add(*head);
            return true;
        }
    };

    struct CompareImmediate
    {
        size_t offset;
        uint8_t size;
    };

    // Decode instructions rather than searching for the byte values 150/200,
    // which can also appear inside addresses or unrelated instructions.
    inline std::optional<CompareImmediate> FindCompareImmediate(std::span<const uint8_t> code, uint32_t value)
    {
        ZydisDecoder decoder{};
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
        std::optional<CompareImmediate> result;
        for (size_t offset = 0; offset < code.size();)
        {
            ZydisDecodedInstruction instruction{};
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, nullptr,
                code.data() + offset, code.size() - offset, &instruction)))
                return std::nullopt;

            const auto& immediate = instruction.raw.imm[0];
            if (instruction.mnemonic == ZYDIS_MNEMONIC_CMP &&
                (immediate.size == 16 || immediate.size == 32) && immediate.value.u == value)
            {
                if (result) // Ambiguous or incompatible code; do not guess a patch site.
                    return std::nullopt;
                result = CompareImmediate{offset + immediate.offset, immediate.size};
            }
            offset += instruction.length;
        }
        return result;
    }
}
