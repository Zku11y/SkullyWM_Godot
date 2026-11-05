"""Guard against confusing advertised support with captured device enablement."""
import importlib.util
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("audit", Path(__file__).resolve().parents[1] / "tools/probe_vulkan.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)

CAPTURE = '''Thread 0, Frame 0:
vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice) returns VkResult VK_SUCCESS (0):
    pCreateInfo: const VkDeviceCreateInfo* = address:
        enabledExtensionCount: uint32_t = 1
        ppEnabledExtensionNames[0]: const char* = "VK_KHR_external_memory_fd"

Thread 0, Frame 0:
vkEnumerateDeviceExtensionProperties():
    extensionName: char[256] = "VK_EXT_external_memory_dma_buf"
'''


class CaptureTests(unittest.TestCase):
    def test_only_enabled_names_are_returned(self):
        self.assertEqual(audit.parse_creation_capture(CAPTURE), ["VK_KHR_external_memory_fd"])

    def test_ambiguous_multiple_devices_rejected(self):
        with self.assertRaises(ValueError):
            audit.parse_creation_capture(CAPTURE + CAPTURE)

    def test_truncated_list_rejected(self):
        with self.assertRaises(ValueError):
            audit.parse_creation_capture(CAPTURE.replace("uint32_t = 1", "uint32_t = 2"))

    def test_missing_or_failed_device_rejected(self):
        for text in ("", CAPTURE.replace("VK_SUCCESS (0)", "VK_ERROR_INITIALIZATION_FAILED (-3)")):
            with self.assertRaises(ValueError):
                audit.parse_creation_capture(text)


if __name__ == "__main__":
    unittest.main()
