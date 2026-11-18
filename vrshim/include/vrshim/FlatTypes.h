#pragma once
// FRIK-flat C11: engine types CommonLibF4VR defines and the flat CommonLibF4 (1.11.240) only forward-declares or lacks.
// Layouts are the flat ones (the `#else` / non-VR branches CommonLibF4VR itself carries) unless marked otherwise.
#include "RE/Fallout.h"

namespace RE
{
    // CommonLibF4VR RE/Bethesda/BSShaderMaterial.h, flat branch: members 0x10 higher than VR (flat base is 0x38).
    // Flat layout: CommonLibF4VR's note says "expected 0x10 higher but unverified" - same status here.
    class __declspec(novtable) BSEffectShaderMaterial : public BSShaderMaterial // 00
    {
    public:
        static constexpr std::uint32_t UNIQUE_HASH_KEY{ 0xFFFFFFFF };

        float falloffStartAngle; // 38
        float falloffStopAngle; // 3C
        float falloffStartOpacity; // 40
        float falloffStopOpacity; // 44
        NiColorA baseColor; // 48
        NiPointer<NiTexture> baseTexture; // 58
        NiPointer<NiTexture> grayscaleTexture; // 60
        NiPointer<NiTexture> envMapTexture; // 68
        NiPointer<NiTexture> envMapMaskTexture; // 70
        NiPointer<NiTexture> normalTexture; // 78
        float softDepth; // 80
        float baseColorScale; // 84
        BSFixedString baseTextureName; // 88
        BSFixedString grayscaleTextureName; // 90
        BSFixedString envMapTextureName; // 98
        BSFixedString envMapMaskTextureName; // A0
        BSFixedString normalTextureName; // A8
        float envMapScale; // B0
        std::uint8_t textureClampMode; // B4
        std::uint8_t lightingInfluence; // B5
        std::uint8_t envMapMinLOD; // B6
        std::uint8_t padB7; // B7
        std::uint32_t unkB8; // B8
    };

    static_assert(sizeof(BSEffectShaderMaterial) == 0xC0);
    static_assert(offsetof(BSEffectShaderMaterial, baseColor) == 0x48);

    namespace BSGraphics
    {
        // Completes the flat lib's forward declaration. Layout = CommonLibF4VR's (srv first); the renderer texture
        // object predates the VR fork, so it is assumed unchanged on 1.11.240 (unverified, see frik-flat-port.md).
        class Texture
        {
        public:
            REX::W32::ID3D11ShaderResourceView* srv; // 00
            REX::W32::ID3D11Texture2D* texture; // 08
            void* unk10; // 10
            void* unk18; // 18
            void* unk20; // 20
        };
    }
}

namespace vrshim
{
    // CommonLibF4VR Actor::RequestDetectionLevel (REL::ID 943772 = 1.10.163 DAA580) -> flat C97060 (func_matches high).
    std::int32_t actorRequestDetectionLevel(RE::Actor* a_observer, RE::Actor* a_target, std::int32_t a_priority = 3);

    // CommonLibF4VR TESObjectARMO::GetSlotMask: the flat BIPED_MODEL is the same first dword.
    inline std::uint32_t armorSlotMask(const RE::TESObjectARMO* a_armo)
    {
        return a_armo->bipedModelData.bipedObjectSlots;
    }

    // CommonLibF4VR BIPED_MODEL::weightClass (+4): not part of the flat lib's 4-byte BIPED_MODEL, read raw.
    inline std::uint32_t armorWeightClass(const RE::TESObjectARMO* a_armo)
    {
        return *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(&a_armo->bipedModelData) + 4);
    }

    enum ArmorWeight : std::uint32_t
    {
        kWeightLight = 0,
        kWeightHeavy = 1,
        kWeightNone = 2
    };
}
