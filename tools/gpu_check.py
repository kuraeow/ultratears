"""Does this PC have a graphics card the game can run on? Asked before the build (tools/setup.py, step check).

The game needs what src/vk/vk.c asks for: Vulkan 1.3 with VK_EXT_shader_object, VK_EXT_external_memory_host and
VK_KHR_swapchain. A Radeon RX 570 (Polaris, Windows) builds for 40 minutes and then stops at start with "Vulkan 1.3
with VK_EXT_shader_object is required" (2026-10-29): AMD's Windows driver for Polaris and Vega has no shader objects,
nor has Intel's for HD 6xx (#17). Read through the Vulkan loader (vulkan-1.dll) with ctypes: nothing to install.

Where no card has shader objects but the Khronos shader object layer (Vulkan SDK) is installed, the cards are looked at
again through it: the game can run that way (a tester's RX 570), slower and not supported.
usage: python tools/gpu_check.py   (prints each graphics card and what it lacks; exit code 1 when none will do)
"""
import ctypes
import sys

NEEDED = ('VK_EXT_shader_object', 'VK_EXT_external_memory_host', 'VK_KHR_swapchain')
API_1_3 = (1 << 22) | (3 << 12)
TYPES = {1: 'integrated', 2: 'discrete', 3: 'virtual', 4: 'CPU'}


class AppInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p), ('pApplicationName', ctypes.c_char_p),
                ('applicationVersion', ctypes.c_uint32), ('pEngineName', ctypes.c_char_p), ('engineVersion', ctypes.c_uint32),
                ('apiVersion', ctypes.c_uint32)]


class InstanceInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p), ('flags', ctypes.c_uint32),
                ('pApplicationInfo', ctypes.POINTER(AppInfo)), ('enabledLayerCount', ctypes.c_uint32),
                ('ppEnabledLayerNames', ctypes.c_void_p), ('enabledExtensionCount', ctypes.c_uint32),
                ('ppEnabledExtensionNames', ctypes.c_void_p)]


class ExtensionProperties(ctypes.Structure):
    _fields_ = [('extensionName', ctypes.c_char * 256), ('specVersion', ctypes.c_uint32)]


LAYER = b'VK_LAYER_KHRONOS_shader_object'


def gpus(layer=None, details=None):
    """[(name, type, 'major.minor.patch', missing requirements)] of every graphics card the Vulkan loader reports, seen
    through layer when given; None when there is no Vulkan at all (or the layer is not installed). details (a list):
    filled with {'vendor': PCI vendor id, 'vram': bytes of the largest device-local heap} per card, in the same order."""
    try:
        vk = ctypes.WinDLL('vulkan-1.dll') if sys.platform == 'win32' else ctypes.CDLL('libvulkan.so.1')
    except OSError:
        return None
    app = AppInfo(0, None, b'UltraTears setup', 0, None, 0, API_1_3)   # VK_STRUCTURE_TYPE_APPLICATION_INFO
    names = (ctypes.c_char_p * 1)(layer)
    ci = InstanceInfo(1, None, 0, ctypes.pointer(app), 1 if layer else 0,   # VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
                      ctypes.cast(names, ctypes.c_void_p) if layer else None, 0, None)
    inst = ctypes.c_void_p()
    if vk.vkCreateInstance(ctypes.byref(ci), None, ctypes.byref(inst)) != 0:
        return None
    try:
        n = ctypes.c_uint32()
        vk.vkEnumeratePhysicalDevices(inst, ctypes.byref(n), None)
        devs = (ctypes.c_void_p * n.value)()
        vk.vkEnumeratePhysicalDevices(inst, ctypes.byref(n), devs)
        out = []
        for d in devs[:n.value]:
            props = ctypes.create_string_buffer(1024)   # VkPhysicalDeviceProperties (824 bytes): apiVersion at 0,
            vk.vkGetPhysicalDeviceProperties(ctypes.c_void_p(d), props)   # deviceType at 16, deviceName at 20
            api = int.from_bytes(props.raw[0:4], 'little')
            kind = int.from_bytes(props.raw[16:20], 'little')
            name = props.raw[20:276].split(b'\0', 1)[0].decode(errors='replace')
            m = ctypes.c_uint32()
            vk.vkEnumerateDeviceExtensionProperties(ctypes.c_void_p(d), None, ctypes.byref(m), None)
            exts = (ExtensionProperties * m.value)()
            vk.vkEnumerateDeviceExtensionProperties(ctypes.c_void_p(d), None, ctypes.byref(m), exts)
            have = {e.extensionName.decode(errors='replace') for e in exts[:m.value]}
            missing = (['Vulkan 1.3'] if api < API_1_3 else []) + [x for x in NEEDED if x not in have]
            out.append((name, TYPES.get(kind, 'other'), f'{api >> 22}.{(api >> 12) & 0x3ff}.{api & 0xfff}', missing))
            if details is not None:
                # VkPhysicalDeviceMemoryProperties: 32 memory types (8 bytes each) after their count, then the heap
                # count at 260 and 16 heaps of {VkDeviceSize size, flags} (16 bytes) from 264; DEVICE_LOCAL is flag bit 0
                mem = ctypes.create_string_buffer(520)
                vk.vkGetPhysicalDeviceMemoryProperties(ctypes.c_void_p(d), mem)
                heaps = int.from_bytes(mem.raw[260:264], 'little')
                local = [int.from_bytes(mem.raw[264 + 16 * h:272 + 16 * h], 'little') for h in range(min(heaps, 16))
                         if int.from_bytes(mem.raw[272 + 16 * h:276 + 16 * h], 'little') & 1]
                details.append({'vendor': int.from_bytes(props.raw[8:12], 'little'), 'vram': max(local, default=0)})
        return out
    finally:
        vk.vkDestroyInstance(inst, None)


def check():
    """(verdict, text): 'ok' a card will do; 'layer' only through the shader object layer; 'no' none; and what to tell
    the player."""
    found = gpus()
    if found is None:
        return 'no', ('no Vulkan driver was found (vulkan-1.dll): install the current driver for your graphics card '
                      '(NVIDIA, AMD or Intel)')
    lines = [f'{name} ({kind}, Vulkan {api})' + (': lacks ' + ', '.join(missing) if missing else ': supported')
             for name, kind, api, missing in found if kind != 'CPU']
    if any(not missing for _, kind, _, missing in found if kind != 'CPU'):
        return 'ok', '; '.join(l for l in lines if l.endswith('supported'))
    layered = gpus(LAYER) or []
    if any(not missing for _, kind, _, missing in layered if kind != 'CPU'):
        return 'layer', ('; '.join(lines) + '. With the Khronos shader object layer (Vulkan SDK) installed here the game can '
                         'run through it: start it with VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_shader_object and '
                         'TOTK_VK_DESCRIPTOR_SETS=1. Slower, possibly with artefacts, and not supported')
    return 'no', ('no graphics card here can run the game: ' + '; '.join(lines or ['none reported']) +
                  '. It needs Vulkan 1.3 with VK_EXT_shader_object: a current driver for NVIDIA (tested on RTX 3060) '
                  'or AMD RX 6000 or newer. AMD RX 400/500 and Vega and Intel HD 600 have no shader '
                  'objects in their Windows drivers. Updating the driver may help; otherwise this PC is not supported yet')


if __name__ == '__main__':
    verdict, text = check()
    print(text)
    sys.exit(0 if verdict == 'ok' else 1)
