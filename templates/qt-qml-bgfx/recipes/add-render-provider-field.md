# Recipe: Add Render Provider Field

1. Add the field to the renderer snapshot.
2. Surface it through `render.resources` or `render.stats`.
3. Add a smoke assertion and, for visual state, pair it with `window.grab`.
4. Verify both `TESTBRIDGE_RENDER_TIER=full` and `TESTBRIDGE_RENDER_TIER=nocompute`
   when the feature should work in both tiers.
5. Update `docs/RENDER_PROVIDER_SDK.md` if it becomes part of the contract.
