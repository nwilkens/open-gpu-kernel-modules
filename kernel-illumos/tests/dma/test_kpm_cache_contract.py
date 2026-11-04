#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Edgecast Cloud LLC.
# SPDX-License-Identifier: MIT
"""Guard against rebuilding an existing KPM PTE in the NVIDIA driver."""

from pathlib import Path
import re
import unittest


SOURCE = Path(__file__).resolve().parents[2] / "nvidia/nvidia_mem.c"


class KpmCacheContract(unittest.TestCase):
    def test_driver_uses_hat_cache_update(self):
        source = SOURCE.read_text()
        body = re.search(r"\nnv_kpm_wc_set\(.*?\n}\n", source, re.S)
        self.assertIsNotNone(body)
        self.assertIn("hat_kpm_set_cache(", body.group())
        self.assertNotIn("hat_devload(", body.group())

    def test_cache_failure_quarantines_dma_pages(self):
        source = SOURCE.read_text()
        self.assertIn("kpm_quarantine", source)
        self.assertNotIn("VERIFY0(hat_kpm_set_cache", source)
        self.assertNotIn("VERIFY(nv_kpm_wc_set", source)


if __name__ == "__main__":
    unittest.main()
