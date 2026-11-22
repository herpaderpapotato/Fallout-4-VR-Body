#pragma once

#include <vector>

#ifdef FRIK_FLAT
#include <chrono>

#include "fo4flatvr/HeadHideCore.h"  // whole-head hide shared with fo4flatvr's AAF POV hide (C39)
#endif

namespace frik
{
    class CullGeometryHandler
    {
    public:
        // hideAll culls every player geometry (scoped without a provider keeping the body visible), bones stay valid
        void cullPlayerGeometry(bool hideAll = false);

#ifdef FRIK_FLAT // C35: the camera left first person (C23): show again what this handler hid (head gear, face/skin, scope cull)
        void restoreAll()
        {
            _isAllCulled = false;
            restoreGeometry();
            restoreEquipment();
            flatHeadless(false, "not first person");
        }

        // C39 (user 19 Nov: "A headless shadow would be better"): in first person the player's whole head is hidden as one unit
        // (FaceGen node + head-slot gear, HeadHideCore.h) instead of FRIK's head push-back; shown again in selfie mode and
        // outside first person. Off: fo4flatvr.ini [FRIK] bHeadlessFirstPerson=0 (FRIK's push-back / FRIK.toml hide flags).
        static bool flatHeadlessOn();
        void flatHeadless(bool a_hide, const char* a_why);

        ~CullGeometryHandler() { _flatHead.Restore(); }
#endif

        // the next cull finds the geometries to hide again instead of using the ones found in the last 2 seconds
        void resetHideGeometryIndexes()
        {
            _lastPreProcessTime = 0;
        }

    private:
        void restoreGeometry();
        void restoreEquipment();
        void preProcessHideGeometryIndexes(RE::BSFadeNode* rn);
        static void setEquipmentSlotByIndexVisibility(int slotId, bool toHide);

        // used to handle update to hide flags to know to restore culled geometries
        bool _isGeometryCulled = false;
        bool _isEquipmentCulled = false;
        bool _isAllCulled = false;

        time_t _lastPreProcessTime = 0;
        int _lastHiddenGeometryIdx = -1;
        std::string _lastHiddenGeometryName;
        std::vector<std::uint32_t> _hideFaceSkinGeometryIndexes;
#ifdef FRIK_FLAT // C39
        fo4flatvr::headhide::Set _flatHead;
        std::chrono::steady_clock::time_point _flatHeadScan{};
        bool _flatHeadNoFaceLogged = false;
#endif
    };
}
