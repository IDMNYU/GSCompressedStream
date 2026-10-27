# GSCompressedStream

Unreal Engine plugin for compressed Gaussian Splatting Video (`.gsv`) playback with SPZ-based intra-frame compression and SH0 mode.

Shared by [IDMNYU](https://github.com/IDMNYU) as part of NSF-supported research software.

## Requirements

- Unreal Engine **5.4+**
- **Niagara** plugin enabled
- A Niagara system asset compatible with the user parameters below
- One or more `.gsv` files on disk

## Install

1. Copy this folder into your project's `Plugins/` directory as `GSCompressedStream`.
2. Open (or regenerate) your `.uproject` and enable **GS Compressed Stream**.
3. Restart the editor if prompted, then build C++ if your project is code-based.

## Usage

1. Add the **GS Compressed Stream** component (`UGSCompressedStream`) to an actor.
2. Set **GSV File Path** (or **File List**) to a `.gsv` path relative to the project directory.
3. Assign **Niagara System Asset** to a system that exposes the expected user parameters.
4. Play in editor or package; optional built-in playback UI and keyboard controls are available on the component.

### Niagara user parameters

The component binds these Niagara user parameters each frame:

| Parameter | Type |
|-----------|------|
| `texture_width` | int |
| `texture_position` | texture |
| `texture_rotation` | texture |
| `texture_scaleAndOpacity` | texture |
| `texture_sh_0` | texture |
| `texture_sh_1` … `texture_sh_11` | texture (optional, higher SH bands) |

This plugin does **not** ship Content. Provide a matching Niagara system from your project or another plugin.

## Module layout

```
GSCompressedStream/
  GSCompressedStream.uplugin
  Source/CompressedStreamer/
    Public/   UGSCompressedStream, GSV format/decoder helpers
    Private/  Implementation
```

## License

MIT — see [LICENSE](LICENSE).
