#include "source/SALodLights/ProceduralLimitsSupport.hpp"
#include <array>
#include <cassert>
#include <iostream>
#include <unordered_set>

using namespace ProceduralLimits;

template<typename T>
size_t Validate(T* head)
{
    std::unordered_set<T*> visited;
    T* previous = nullptr;
    for (auto* item = head; item; item = item->next)
    {
        assert(visited.insert(item).second); // No cycles or duplicate insertion.
        assert(item->prev == previous);
        previous = item;
    }
    return visited.size();
}

template<typename T>
T* Pop(T*& head)
{
    auto* item = head;
    if (item)
    {
        head = item->next;
        if (head)
            head->prev = nullptr;
    }
    return item;
}

ObjectEntry* Pop(CountedList<ObjectEntry>& list)
{
    auto* item = Pop(list.head);
    if (item)
    {
        --list.count;
        if (!list.head)
            list.tail = nullptr;
    }
    return item;
}

void Validate(const CountedList<ObjectEntry>& list)
{
    assert(Validate(list.head) == list.count);
    auto* last = list.head;
    while (last && last->next)
        last = last->next;
    assert(last == list.tail);
}

void TestObjectPoolLifecycle()
{
    std::array<ObjectEntry, 512> original{};
    std::array<ObjectEntry, 512> extra{};
    CountedList<ObjectEntry> unused;
    SurfaceInfo surface{};

    for (int game = 0; game < 20; ++game)
    {
        // Original Init rebuilds its own list; the hook then adds the extension.
        AttachUnused(unused, std::span<ObjectEntry>{original});
        AttachUnused(unused, std::span<ObjectEntry>{extra});
        Validate(unused);
        assert(unused.count == 1024);

        while (auto* item = Pop(unused))
            surface.objects.Add(*item);
        Validate(surface.objects);
        assert(surface.objects.count == 1024);
        assert(!unused.head && !unused.tail && unused.count == 0);

        // Original Exit returns all active entries, then clears the free-list heads.
        while (auto* item = Pop(surface.objects))
            unused.Add(*item);
        Validate(unused);
        assert(unused.count == 1024);
        unused = {};
    }
}

template<typename T, size_t Original, size_t Extra>
void TestTerrainPoolLifecycle()
{
    std::array<T, Original> original{};
    std::array<T, Extra> extra{};
    T* unused = nullptr;
    T* active = nullptr;

    for (int reload = 0; reload < 20; ++reload)
    {
        // Release entries before resetting storage, including on a live reload.
        while (auto* item = Pop(active))
            Prepend(unused, *item);
        unused = nullptr;
        AttachUnused(unused, std::span<T>{original});
        AttachUnused(unused, std::span<T>{extra});
        assert(Validate(unused) == Original + Extra);

        for (size_t i = 0; i < Original + Extra; ++i)
        {
            auto* item = Pop(unused);
            assert(item);
            Prepend(active, *item);
        }
        assert(!unused);
        assert(Validate(active) == Original + Extra);
    }
}

void TestAllocationRecovery()
{
    std::array<ObjectEntry, 2> storage{};
    SurfaceInfo surface{};
    CountedList<ObjectEntry> unused;
    AttachUnused(unused, std::span<ObjectEntry>{storage});

    // Matrix/temp-object rejection after RemoveHead must not leak capacity.
    for (int failure = 0; failure < 1000; ++failure)
    {
        const AllocationSnapshot before(unused);
        assert(Pop(unused) == before.head);
        assert(before.RecoverFailure(unused, surface));
        assert(!before.RecoverFailure(unused, surface));
        Validate(unused);
        assert(unused.count == 2);
    }

    // A mod which already returns the failed entry must not be double-fixed.
    const AllocationSnapshot returned(unused);
    auto* item = Pop(unused);
    unused.Add(*item);
    assert(!returned.RecoverFailure(unused, surface));

    // Never recover a node which acquired an entity or joined an active list.
    const AllocationSnapshot successful(unused);
    item = Pop(unused);
    int entity;
    item->object = &entity;
    assert(!successful.RecoverFailure(unused, surface));
    item->object = nullptr;
    surface.objects.Add(*item);
    assert(!successful.RecoverFailure(unused, surface));
    unused.Add(*Pop(surface.objects));

    // Removing/recovering the last free entry must restore tail and count, too.
    auto* held = Pop(unused);
    const AllocationSnapshot last(unused);
    assert(Pop(unused) == last.head);
    assert(last.RecoverFailure(unused, surface));
    Validate(unused);
    assert(unused.head == unused.tail && unused.count == 1);
    unused.Add(*held);
    Validate(unused);

    CountedList<ObjectEntry> empty;
    assert(!AllocationSnapshot(empty).RecoverFailure(empty, surface));
}

void TestComparisonDecoding()
{
    // Use both SA's possible word and dword CMP encodings. Values in MOV/PUSH
    // or displacement bytes must not be interpreted as comparison immediates.
    const std::array<uint8_t, 29> code{
        0xB8, 0x96, 0x00, 0x00, 0x00,                   // mov eax,150
        0x66, 0x81, 0x3D, 0x70, 0x4A, 0xBB, 0x00, 0x96, 0x00, // cmp word [BB4A70],150
        0x81, 0x3D, 0xB0, 0x7C, 0xBB, 0x00, 0xC8, 0x00, 0x00, 0x00, // cmp [BB7CB0],200
        0x68, 0xC8, 0x00, 0x00, 0x00                    // push 200
    };
    const auto temp = FindCompareImmediate(code, 150);
    const auto matrices = FindCompareImmediate(code, 200);
    assert(temp && temp->size == 16 && temp->offset == 12);
    assert(matrices && matrices->size == 32 && matrices->offset == 20);
    assert(!FindCompareImmediate(code, 400));

    const std::array<uint8_t, 10> ambiguous{
        0x3D, 0x96, 0, 0, 0, 0x3D, 0x96, 0, 0, 0
    };
    assert(!FindCompareImmediate(ambiguous, 150));
    const std::array<uint8_t, 2> truncated{0x81, 0x3D};
    assert(!FindCompareImmediate(truncated, 150));
}

int main()
{
    Settings defaults;
    defaults.Normalize();
    assert(defaults.objectInstances == 512 && defaults.terrainTriangles == 256 &&
        defaults.collisionEntities == 40 && defaults.temporaryObjects == 150 && defaults.matrices == 200);
    Settings invalid{-1, 0, -100, 1000000, 1000000};
    invalid.Normalize();
    assert(invalid.objectInstances == 512 && invalid.terrainTriangles == 256 &&
        invalid.collisionEntities == 40 && invalid.temporaryObjects == 65535 && invalid.matrices == 65535);

    TestObjectPoolLifecycle();
    TestTerrainPoolLifecycle<PlantTriangle, 256, 256>();
    TestTerrainPoolLifecycle<CollisionEntry, 40, 40>();
    TestAllocationRecovery();
    TestComparisonDecoding();
    std::cout << "Procedural limits: lifecycle, allocation recovery and instruction decoding passed.\n";
}
