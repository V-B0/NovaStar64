# Third-party notices

NovaStar64 is licensed under the GNU General Public License, version 2 (see [LICENSE](LICENSE)). It contains or
links the software below, each under its own licence. The versions are the ones pinned in
[`nova64/cmake/Dependencies.cmake`](nova64/cmake/Dependencies.cmake). Each project's own licence file, at that
version, is the authority; the texts of the common licences are repeated at the end.

An installed copy also carries its licences inside the program: on macOS in
`NovaStar64.app/Contents/Resources/Licenses`, on Linux in `usr/share/licenses/novastar64` inside the AppImage.

## Project64

The CPU core, the recompilers and the RSP began as a fork of [Project64](https://github.com/project64/project64)
(GPL-2.0, copyright the Project64 team). Files that carry its code say so in their header; those headers must stay.

## Components

| Component | Version | Used for | Licence | Copyright holder |
|---|---|---|---|---|
| [AsmJit](https://github.com/asmjit/asmjit) | commit 5134d39 | Recompiler code generation | Zlib | The AsmJit authors |
| [SDL2](https://github.com/libsdl-org/SDL) | 2.30.2 | Windowing, input, audio | Zlib | Sam Lantinga and the SDL contributors |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | 1.3.275 | Vulkan API | Apache-2.0 or MIT | The Khronos Group Inc. |
| [volk](https://github.com/zeux/volk) | 1.3.270 | Vulkan loader | MIT | Arseny Kapoulkine |
| [glslang](https://github.com/KhronosGroup/glslang) | 14.2.0 | GLSL to SPIR-V for the Vulkan backend | BSD-3-Clause, with parts under other permissive licences (see its `LICENSE.txt`) | The Khronos Group, Google, LunarG, NVIDIA, 3Dlabs and others |
| [MoltenVK](https://github.com/KhronosGroup/MoltenVK) | prebuilt, macOS only | Vulkan on Metal | Apache-2.0 | The Brenwill Workshop Ltd. |
| [glad](https://github.com/Dav1dde/glad) 2.0 | generated, in `nova64/external/glad/` | OpenGL loader | (WTFPL or CC0-1.0) and Apache-2.0; `khrplatform.h` is Khronos MIT-style | David Herberth; The Khronos Group |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.91.8 | Launcher interface | MIT | Omar Cornut |
| [Inter](https://rsms.me/inter/) | 4.1 | Launcher text (cut down to the glyphs used) | SIL Open Font License 1.1 | The Inter Project Authors |
| [Phosphor Icons](https://phosphoricons.com/) | | Launcher icons (cut down to the icons used) | MIT | Phosphor Icons |
| [stb](https://github.com/nothings/stb) | commit 2c980bb | Image reading and writing | MIT or public domain | Sean Barrett and contributors |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | JSON | MIT | Niels Lohmann |
| [rcheevos](https://github.com/RetroAchievements/rcheevos) | 11.5.0 | RetroAchievements (not yet wired up) | MIT | RetroAchievements |
| [Lua](https://www.lua.org/) | 5.4.7 | Scripting | MIT | Lua.org, PUC-Rio |
| [Berkeley SoftFloat](http://www.jhauser.us/arithmetic/SoftFloat.html) | 3e | FPU arithmetic | BSD-3-Clause | The Regents of the University of California |

The licence files that ship in this repository: [`nova64/thirdparty/MoltenVK/LICENSE`](nova64/thirdparty/MoltenVK/LICENSE),
[`nova64/assets/fonts/LICENSE-Inter-OFL.txt`](nova64/assets/fonts/LICENSE-Inter-OFL.txt) and
[`nova64/assets/fonts/LICENSE-Phosphor-MIT.txt`](nova64/assets/fonts/LICENSE-Phosphor-MIT.txt).

## Licence texts

**Apache License 2.0** — the full text is at <https://www.apache.org/licenses/LICENSE-2.0.txt> and in
`nova64/thirdparty/MoltenVK/LICENSE`.

**SIL Open Font License 1.1** — the full text is in `nova64/assets/fonts/LICENSE-Inter-OFL.txt`.

**MIT licence**

> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
> documentation files (the "Software"), to deal in the Software without restriction, including without limitation
> the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
> to permit persons to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of
> the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
> THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
> CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
> IN THE SOFTWARE.

**zlib licence**

> This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held
> liable for any damages arising from the use of this software.
>
> Permission is granted to anyone to use this software for any purpose, including commercial applications, and to
> alter it and redistribute it freely, subject to the following restrictions:
>
> 1. The origin of this software must not be misrepresented; you must not claim that you wrote the original
>    software. If you use this software in a product, an acknowledgment in the product documentation would be
>    appreciated but is not required.
> 2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original
>    software.
> 3. This notice may not be removed or altered from any source distribution.

**BSD 3-Clause licence**

> Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
> following conditions are met:
>
> 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the
>    following disclaimer.
> 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
>    following disclaimer in the documentation and/or other materials provided with the distribution.
> 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
>    products derived from this software without specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
> WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
> PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
> INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
> SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
> THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
> ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
