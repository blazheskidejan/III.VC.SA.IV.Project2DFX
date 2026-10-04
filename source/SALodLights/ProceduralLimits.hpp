#pragma once

#include "ProceduralLimitsSupport.hpp"
#include <fstream>
#include <vector>

namespace ProceduralLimits
{
    // Layouts and addresses for SA 1.0 US, as used by the other SA hooks.
    // See gta-reversed: Plant/{ProcObjectMan,ProcSurfaceInfo,PlantLocTri,
    // PlantColEntEntry}.h and PlantMgr.cpp.
    static_assert(sizeof(Vector) == 0xC);
    static_assert(sizeof(PlantTriangle) == 0x54);
    static_assert(offsetof(PlantTriangle, next) == 0x4C);
    static_assert(sizeof(CollisionEntry) == 0x14);
    static_assert(offsetof(CollisionEntry, next) == 0xC);
    static_assert(sizeof(ObjectEntry) == 0x14);
    static_assert(offsetof(ObjectEntry, object) == 0x8);
    static_assert(sizeof(CountedList<ObjectEntry>) == 0xC);
    static_assert(sizeof(SurfaceInfo) == 0x48);
    static_assert(offsetof(ObjectManager, unused) == 0x4C08);
    static_assert(sizeof(ObjectManager) == 0x4C14);

    inline std::vector<ObjectEntry> extraObjects;
    inline std::vector<PlantTriangle> extraTriangles;
    inline std::vector<CollisionEntry> extraCollisionEntries;
    inline SafetyHookInline shObjectManagerInit;
    inline SafetyHookInline shPlantReloadConfig;
    inline SafetyHookInline shAddObject;
    inline SafetyHookInline shObjectManagerExit;
    inline uint32_t peakObjects = 0;
    inline uint64_t failedCreations = 0;
    inline uint64_t recoveredEntries = 0;

    inline ObjectManager& Manager()
    {
        return *reinterpret_cast<ObjectManager*>(0xBB7CB0);
    }

    inline CollisionEntry*& ActiveCollisionEntries()
    {
        return *reinterpret_cast<CollisionEntry**>(0xC0399C);
    }

    inline std::ofstream& Log()
    {
        static std::ofstream stream([]
        {
            CIniReader reader("");
            auto path = reader.GetIniPath();
            path.replace_extension(".procedural.log");
            return path;
        }());
        return stream;
    }

    inline void __fastcall ObjectManagerInit(ObjectManager* manager, void*)
    {
        shObjectManagerInit.unsafe_thiscall<void>(manager);
        // The original Init adds its 512 nodes; Exit clears the list between games.
        AttachUnused(manager->unused, std::span{extraObjects});
        peakObjects = 0;
        failedCreations = 0;
        recoveredEntries = 0;
    }

    inline void __fastcall ObjectManagerExit(ObjectManager* manager, void*)
    {
        Log() << "Procedural objects: peak " << peakObjects << '/' << (512 + extraObjects.size())
              << ", failed creations " << failedCreations
              << ", recovered tracking entries " << recoveredEntries << std::endl;
        // Walks all surface lists, including our entries, deletes their entities,
        // then clears the free list. Keep the backing storage for the next Init.
        shObjectManagerExit.unsafe_thiscall<void>(manager);
    }

    inline bool __cdecl PlantReloadConfig()
    {
        // ReloadConfig rebuilds list heads. Release active collision entries first
        // so their triangle arrays, procedural entities and entity references cannot
        // be orphaned. Shutdown normally already does this; an empty list is fine.
        while (auto* entry = ActiveCollisionEntries())
            reinterpret_cast<void(__thiscall*)(CollisionEntry*)>(0x5DB8A0)(entry);

        if (!shPlantReloadConfig.unsafe_ccall<bool>())
            return false;

        auto& unusedTriangles = *reinterpret_cast<PlantTriangle**>(0xC03984);
        auto& unusedCollisionEntries = *reinterpret_cast<CollisionEntry**>(0xC03998);
        AttachUnused(unusedTriangles, std::span{extraTriangles});
        AttachUnused(unusedCollisionEntries, std::span{extraCollisionEntries});
        return true;
    }

    inline ObjectEntry* __fastcall AddObject(SurfaceInfo* surface, void*, Vector position, Vector normal, uint8_t lighting)
    {
        auto& unused = Manager().unused;
        const AllocationSnapshot before(unused);
        auto* result = shAddObject.unsafe_thiscall<ObjectEntry*>(surface, position, normal, lighting);
        if (!result)
        {
            ++failedCreations;
            if (before.RecoverFailure(unused, *surface))
                ++recoveredEntries;
        }
        const auto capacity = static_cast<uint32_t>(512 + extraObjects.size());
        if (unused.count <= capacity)
            peakObjects = std::max(peakObjects, capacity - unused.count);
        return result;
    }

    inline void Install(Settings settings)
    {
        settings.Normalize();
        if (settings.objectInstances == 512 && settings.terrainTriangles == 256 &&
            settings.collisionEntities == 40 && settings.temporaryObjects == 150 && settings.matrices == 200)
            return;

        // Locate both checks before changing code. A word-sized immediate is kept
        // word-sized; MatrixList/Objects in OLA are independent of these checks.
        auto* addObjectCode = reinterpret_cast<uint8_t*>(0x5A32D0);
        const std::span<const uint8_t> code(addObjectCode, 0x5A3850 - 0x5A32D0);
        const auto tempCheck = FindCompareImmediate(code, 150);
        const auto matrixCheck = FindCompareImmediate(code, 200);
        if ((settings.temporaryObjects != 150 && !tempCheck) || (settings.matrices != 200 && !matrixCheck))
        {
            Log() << "Not installed: procedural comparison instructions do not match SA 1.0 US." << std::endl;
            return;
        }

        // Allocate once at plugin initialization; never resize linked storage.
        extraObjects.resize(settings.objectInstances - 512);
        extraTriangles.resize(settings.terrainTriangles - 256);
        extraCollisionEntries.resize(settings.collisionEntities - 40);

        shObjectManagerInit = safetyhook::create_inline(0x5A3EA0, ObjectManagerInit);
        shObjectManagerExit = safetyhook::create_inline(0x5A3EE0, ObjectManagerExit);
        shPlantReloadConfig = safetyhook::create_inline(0x5DD780, PlantReloadConfig);
        shAddObject = safetyhook::create_inline(0x5A32D0, AddObject);
        if (!shObjectManagerInit || !shObjectManagerExit || !shPlantReloadConfig || !shAddObject)
        {
            shAddObject.reset();
            shPlantReloadConfig.reset();
            shObjectManagerExit.reset();
            shObjectManagerInit.reset();
            Log() << "Not installed: could not create procedural initialization/allocation hooks." << std::endl;
            return;
        }

        const auto writeLimit = [addObjectCode](const CompareImmediate& check, int value)
        {
            if (check.size == 16)
                injector::WriteMemory<uint16_t>(addObjectCode + check.offset, static_cast<uint16_t>(value), true);
            else
                injector::WriteMemory<uint32_t>(addObjectCode + check.offset, static_cast<uint32_t>(value), true);
        };
        if (settings.temporaryObjects != 150)
            writeLimit(*tempCheck, settings.temporaryObjects);
        if (settings.matrices != 200)
            writeLimit(*matrixCheck, settings.matrices);

        Log() << "Installed capacities: objects=" << settings.objectInstances
              << ", triangles=" << settings.terrainTriangles
              << ", collision entities=" << settings.collisionEntities
              << ", temporary objects=" << settings.temporaryObjects
              << ", matrices=" << settings.matrices << std::endl;
    }
}
