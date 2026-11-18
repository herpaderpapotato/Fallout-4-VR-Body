// Generated from external/openvr/openvr.h (IVRSystem_019) by hand-run script in frik-flat-port.md: every IVRSystem method
// FRIK-flat does not implement returns a neutral value. Included inside class FlatVRSystem (OpenVRShim.cpp).
void GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) override
{}

HmdMatrix44_t GetProjectionMatrix(EVREye eEye, float fNearZ, float fFarZ) override
{
    return {};
}

void GetProjectionRaw(EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) override
{}

bool ComputeDistortion(EVREye eEye, float fU, float fV, DistortionCoordinates_t* pDistortionCoordinates) override
{
    return false;
}

HmdMatrix34_t GetEyeToHeadTransform(EVREye eEye) override
{
    return {};
}

int32_t GetD3D9AdapterIndex() override
{
    return {};
}

void GetDXGIOutputInfo(int32_t* pnAdapterIndex) override
{}

void GetOutputDevice(uint64_t* pnDevice, ETextureType textureType, VkInstance_T* pInstance) override
{}

bool IsDisplayOnDesktop() override
{
    return false;
}

bool SetDisplayVisibility(bool bIsVisibleOnDesktop) override
{
    return false;
}

void ResetSeatedZeroPose() override
{}

HmdMatrix34_t GetSeatedZeroPoseToStandingAbsoluteTrackingPose() override
{
    return {};
}

HmdMatrix34_t GetRawZeroPoseToStandingAbsoluteTrackingPose() override
{
    return {};
}

uint32_t GetSortedTrackedDeviceIndicesOfClass(ETrackedDeviceClass eTrackedDeviceClass, vr::TrackedDeviceIndex_t* punTrackedDeviceIndexArray,
    uint32_t unTrackedDeviceIndexArrayCount, vr::TrackedDeviceIndex_t unRelativeToTrackedDeviceIndex = k_unTrackedDeviceIndex_Hmd) override
{
    return {};
}

EDeviceActivityLevel GetTrackedDeviceActivityLevel(vr::TrackedDeviceIndex_t unDeviceId) override
{
    return {};
}

void ApplyTransform(TrackedDevicePose_t* pOutputPose, const TrackedDevicePose_t* pTrackedDevicePose, const HmdMatrix34_t* pTransform) override
{}

bool GetBoolTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError* pError) override
{
    return false;
}

float GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError* pError) override
{
    return {};
}

int32_t GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError* pError) override
{
    return {};
}

uint64_t GetUint64TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError* pError) override
{
    return {};
}

HmdMatrix34_t GetMatrix34TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError* pError) override
{
    return {};
}

uint32_t GetArrayTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, PropertyTypeTag_t propType, void* pBuffer, uint32_t unBufferSize,
    ETrackedPropertyError* pError) override
{
    return {};
}

uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, char* pchValue, uint32_t unBufferSize,
    ETrackedPropertyError* pError) override
{
    return {};
}

const char* GetPropErrorNameFromEnum(ETrackedPropertyError error) override
{
    return nullptr;
}

bool PollNextEvent(VREvent_t* pEvent, uint32_t uncbVREvent) override
{
    return false;
}

bool PollNextEventWithPose(ETrackingUniverseOrigin eOrigin, VREvent_t* pEvent, uint32_t uncbVREvent, vr::TrackedDevicePose_t* pTrackedDevicePose) override
{
    return false;
}

const char* GetEventTypeNameFromEnum(EVREventType eType) override
{
    return nullptr;
}

HiddenAreaMesh_t GetHiddenAreaMesh(EVREye eEye, EHiddenAreaMeshType type) override
{
    return {};
}

const char* GetButtonIdNameFromEnum(EVRButtonId eButtonId) override
{
    return nullptr;
}

const char* GetControllerAxisTypeNameFromEnum(EVRControllerAxisType eAxisType) override
{
    return nullptr;
}

bool IsSteamVRDrawingControllers() override
{
    return false;
}

bool ShouldApplicationPause() override
{
    return false;
}

bool ShouldApplicationReduceRenderingWork() override
{
    return false;
}

uint32_t DriverDebugRequest(vr::TrackedDeviceIndex_t unDeviceIndex, const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize) override
{
    return {};
}

vr::EVRFirmwareError PerformFirmwareUpdate(vr::TrackedDeviceIndex_t unDeviceIndex) override
{
    return {};
}

void AcknowledgeQuit_Exiting() override
{}

void AcknowledgeQuit_UserPrompt() override
{}
