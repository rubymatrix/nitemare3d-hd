# Gothic HUD kit

Reconstructed from the approved HUD concept using the built-in image generator; these are visual reconstructions, not pixel-identical source extractions.

Eight individually trimmed RGBA PNG sprites: portrait-frame (empty opening), health-frame, health-fill, two ammo ornaments, bullet, gold key and silver key. The original key indicators have no enclosing frame. All transparent pixels have zero RGB. See assets.json for sizes.

The portrait itself is intentionally omitted so animated game portraits can sit behind the frame. Scale and position the health fill inside the health frame and clip its width at runtime. Anchor the two ammo ornaments independently around dynamic text.

Font: gothic-font.png with BMFont text descriptor gothic-font.fnt, font-metrics.json and 64 individual glyph PNGs. Contains A-Z, a-z, 0-9, slash and percent plus a space metric. This is a bitmap font, not a TTF/OTF. Glyph spacing is automatically estimated; tune kerning/baselines for your target game size. Unreal may require a bitmap-font importer or a custom atlas text widget. It is not an engine-import-tested Font asset.

Preview JPGs show real transparency composited over gray. The PNGs contain actual alpha, not a painted checkerboard.

Generation prompts: faithful modular transparent reconstruction of the approved tarnished-silver gothic portrait frame, health frame and crimson fill, ammo ornaments, brass bullet, gold/silver keys; separate 16-by-4 atlas matching the reference ivory condensed gothic serif HUD digits, containing the 64 glyphs listed above. Built-in image generation was used; individual sprites and glyphs were then cropped and packaged.

## Item expansion
Seven additional transparent sprites: spell book, single silver bullet, three silver bullets, red key, green key, red keycard and yellow keycard. Existing gold/silver keys and brass ammo are preserved. Generated using the built-in image tool with the original kit as style reference; prompt requested isolated gothic pixel-art items with matching aged metal, saturated red/green key variants and red/yellow chipped access cards.
