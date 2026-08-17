# Material

Each color entry in the [palette](Palette.md) can have several material properties. The voxel-grid path tracer uses **emit** (a lamp on solids; a softer in-volume glow on volumetric), **Boost emit** (Magica `_flux`, extra punch when emit is up), **metal** / **roughness** / **specular**, **alpha** / **Glass** / **Blend**, **indexOfRefraction**, **attenuation**, and **Volumetric** (**Density**, **Scatter**, **Rim light**) for cloud, fog, smoke, and dust. **Scatter** is how much environment light the volume shows (smoke vs cloud). It is not emit. Checking **Volumetric** stores Magica type Media. `ldr` and `sp` are kept for import/export but hidden in the palette popup.

## Materials

> The material support in vengi is modelled after magicavoxel.

The following material names are imported from magicavoxel and a few of them are exported to the GLTF-[format](Formats.md).

| Material name         | GLTF mapping                                               |
| --------------------- | ---------------------------------------------------------- |
| `metal`               | pbrMetallicRoughness.metallicFactor                        |
| `roughness`           | pbrMetallicRoughness.roughnessFactor                       |
| `specular`            | KHR_materials_specular (fallback: KHR_materials_pbrSpecularGlossiness) |
| `indexOfRefraction`   | KHR_materials_ior                                          |
| `attenuation`         | KHR_materials_volume.attenuationDistance (= 1 / attenuation) |
| `flux`                |                                                            |
| `emit`                | emissiveFactor                                             |
| `lowDynamicRange`     |                                                            |
| `density`             |                                                            |
| `sp`                  |                                                            |
| `phase`               |                                                            |
| `media`               |                                                            |

MagicaVoxel `MaterialType` (Diffuse / Metal / Glass / Emit / Blend / Media) has no stock glTF equivalent and is not reconstructed on import.

You can also modify these values via [scripting](LUAScript.md).

## GLTF extensions

Some of the material properties are exported to GLTF 2.0 or some of the extensions:

* [KHR_materials_ior](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_ior)
* [KHR_materials_volume](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_volume)
* [KHR_materials_specular](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_specular)
* [KHR_materials_pbrSpecularGlossiness](https://kcoley.github.io/glTF/extensions/2.0/Khronos/KHR_materials_pbrSpecularGlossiness) (optional fallback for specular; off by default)
* [KHR_materials_emissive_strength](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_emissive_strength) (read on import for HDR scale; MagicaVoxel emit 0..1 uses core emissiveFactor only)
