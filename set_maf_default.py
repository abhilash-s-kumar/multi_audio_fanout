import comtypes
import warnings
from comtypes import COMMETHOD, GUID, HRESULT, IUnknown
from ctypes import c_int, c_wchar_p
from pycaw.pycaw import AudioUtilities


class IPolicyConfig(IUnknown):
    _iid_ = GUID("{f8679f50-850a-41cf-9c72-430f290290c8}")
    _methods_ = [
        COMMETHOD([], HRESULT, "GetMixFormat"),
        COMMETHOD([], HRESULT, "GetDeviceFormat"),
        COMMETHOD([], HRESULT, "ResetDeviceFormat"),
        COMMETHOD([], HRESULT, "SetDeviceFormat"),
        COMMETHOD([], HRESULT, "GetProcessingPeriod"),
        COMMETHOD([], HRESULT, "SetProcessingPeriod"),
        COMMETHOD([], HRESULT, "GetShareMode"),
        COMMETHOD([], HRESULT, "SetShareMode"),
        COMMETHOD([], HRESULT, "GetPropertyValue"),
        COMMETHOD([], HRESULT, "SetPropertyValue"),
        COMMETHOD(
            [],
            HRESULT,
            "SetDefaultEndpoint",
            (["in"], c_wchar_p, "wszDeviceId"),
            (["in"], c_int, "role"),
        ),
        COMMETHOD([], HRESULT, "SetEndpointVisibility"),
    ]


def main() -> None:
    warnings.filterwarnings("ignore", category=UserWarning, module="pycaw.utils")
    # GetSpeakers returns the current default, so enumerate all active render
    # endpoints through pycaw's private helper instead.
    target = None
    for device in AudioUtilities.GetAllDevices():
        name = getattr(device, "FriendlyName", "")
        endpoint_id = getattr(device, "id", "")
        if name == "Speakers (MAF Virtual Audio Output)" and endpoint_id.startswith("{0.0.0."):
            target = endpoint_id
            break

    if not target:
        raise SystemExit("Could not find Speakers (MAF Virtual Audio Output)")

    policy = comtypes.CoCreateInstance(
        GUID("{870af99c-171d-4f9e-af0d-e63df40c2bc9}"),
        interface=IPolicyConfig,
        clsctx=comtypes.CLSCTX_ALL,
    )
    for role in (0, 1, 2):
        policy.SetDefaultEndpoint(target, role)
    print(f"Default render endpoint set to {target}")


if __name__ == "__main__":
    main()
