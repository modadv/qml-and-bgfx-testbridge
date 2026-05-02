# Recipe: Golden Image Oracle

1. Set `TESTBRIDGE_GOLDEN_BGFX_REGION`.
2. Set `TESTBRIDGE_UPDATE_GOLDEN=1` only for intentional visual changes.
3. Run the smoke test.
4. Re-run without update mode.
5. Inspect `actual_crop.png`, `expected.png`, `diff.png`, and `metrics.json` on failure.
