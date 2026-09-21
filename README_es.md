# r800zzXRdlnaServer

[English](README.md) | [Русский](README_ru.md) | [Español](README_es.md) | [ภาษาไทย](README_th.md) | [中文](README_zh.md) | [한국어](README_ko.md) | [日本語](README_ja.md)

Un servidor DLNA para Windows destinado a la reproducción de vídeo XR en PICO, Meta Quest y otros dispositivos compatibles, con eliminación de fondo mediante IA en tiempo real (RVM), salida alfa/croma y conversión de vídeo.

**Se requiere una GPU NVIDIA para el passthrough de IA en tiempo real.**

`r800zzXRdlnaServer` es una aplicación nativa de C++. Usa directamente las bibliotecas FFmpeg y no ejecuta Python ni `ffmpeg.exe`.

## Descarga

https://github.com/r800zz/r800zzxrdlnaserver/releases/latest

### Reproductores de vídeo VR compatibles con la salida

Para MP4 con pantalla verde, algunos ejemplos de reproductores VR con función de croma son:

- [R800ZZbrowser para PICO/Meta](https://vr180g.com/browser/browser.php?l=en)
- [r800zzvrplayer para PICO 4 Ultra/PICO4](https://vr180g.com/pico/vrplayer.php?l=en)

Para Alpha Packed (formato de vídeo alfa de DeoVR):

- [r800zzvrplayer para PICO 4 Ultra/PICO4](https://vr180g.com/pico/vrplayer.php?l=en)
- DeoVR (No admite vídeos que no sean VR.)

Para WebM VP9 Alpha, el reproductor VR que he confirmado es:

- **[r800zzvrplayer 0.6 o posterior](https://vr180g.com/pico/vrplayer.php?l=en)**

## Funciones

- Sirve mediante DLNA una carpeta de vídeos seleccionada y archivos de vídeo seleccionados individualmente.
- Conserva el nombre exacto del archivo de origen en la lista multimedia DLNA.
- Proporciona descubrimiento UPnP/DLNA y compatibilidad con ContentDirectory para clientes compatibles, incluido DeoVR.
- Elimina fondos de vídeo en tiempo real con Robust Video Matting (RVM).
- Admite Alpha Packed, alfa WebM VP9 auténtico y salida con croma verde.
- Incluye conversión de vídeo sin conexión con aceleración mediante GPU NVIDIA y una alternativa por CPU.
- Incluye un reproductor de vídeo sencillo para Windows con audio, búsqueda, pausa/reanudación, pantalla completa y visualización de la mitad izquierda de SBS.
- Detecta automáticamente la disponibilidad de NVIDIA CUDA/RVM/NVENC.
- Ofrece interfaces en inglés, ruso, español, tailandés, chino, coreano y japonés.
- Guarda la configuración de cada usuario en `%LOCALAPPDATA%`, no en el directorio de instalación.

## Modos de salida DLNA

| Modo | Salida | Notas |
| --- | --- | --- |
| Alpha Packed | HEVC/NVENC en MPEG-TS | Modo rápido en tiempo real para reproductores XR compatibles. |
| Alpha WebM VP9 | WebM con alfa VP9 auténtico | Usa codificación libvpx basada en CPU y puede omitir fotogramas, especialmente con vídeo 4K/60. |
| Chroma Key | HEVC/NVENC con fondo verde | Use un modo de croma verde en el reproductor receptor. |
| OFF | Archivo de origen original | Sin procesamiento RVM y sin necesidad de GPU NVIDIA. |

La interpretación del alfa depende del reproductor receptor. Un cliente que no admita el formato alfa seleccionado puede mostrar una imagen opaca.

## Requisitos del sistema

### Versión precompilada

- Windows 10 o Windows 11, de 64 bits
- Una red local privada compartida por el PC con Windows y el dispositivo de reproducción
- Un reproductor de vídeo DLNA compatible
- Una GPU NVIDIA compatible y un controlador NVIDIA actualizado para los modos de IA en tiempo real

El paquete precompilado contiene los componentes de ejecución necesarios para la aplicación. No es necesario instalar por separado CUDA Toolkit para el uso normal de la aplicación empaquetada. El controlador de pantalla de NVIDIA sigue siendo necesario para utilizar la GPU.

Sin una GPU NVIDIA compatible:

- La entrega DLNA normal sigue disponible en el modo `OFF`.
- La conversión RVM sin conexión puede usar el modelo ONNX FP32 mediante la alternativa por CPU.
- Los modos Alpha Packed, Alpha WebM VP9 y Chroma Key en tiempo real quedan desactivados.

## Instalación

1. Descargue el instalador más reciente para Windows x64 desde la página **Releases** del repositorio.
2. Ejecute el instalador.
3. Inicie `r800zzXRdlnaServer`.
4. Si el Firewall de Windows solicita permiso, permita el acceso en **Redes privadas**.

El instalador es relativamente grande porque la compilación para GPU incluye compatibilidad de ONNX Runtime con CUDA, componentes de ejecución de CUDA/cuDNN, bibliotecas FFmpeg y los modelos RVM.

## Uso básico de DLNA

1. Seleccione una carpeta con **Select Video Folder...**, añada archivos individuales con **Select Video File...**, o use ambas opciones.
2. Seleccione un modo de salida:
   - **Alpha packed**
   - **Alpha WebM VP9**
   - **Chroma Key**
   - **OFF**
3. Confirme la dirección IPv4 anunciada.
4. Seleccione **Start DLNA Server**.
5. Abra un reproductor DLNA compatible en la misma red local.
6. Seleccione `r800zzXRdlnaServer` y elija un vídeo.

El registro del servidor muestra el descubrimiento DLNA, las solicitudes de descripción, las solicitudes de lista multimedia, las solicitudes de reproducción y los errores del proceso worker. **Log Clear** borra el registro mostrado.

## Conversión de vídeo

El conversor integrado puede crear:

- MP4 con croma
- Alpha WebM VP9
- MP4 Alpha Packed

El nombre sugerido del archivo de salida incluye la hora de inicio de la conversión:

```text
movie_RVM_ChromaKey_202609210542.mp4
```

El progreso de la conversión se muestra como un contador de fotogramas de salida. El conversor escribe primero en un archivo parcial interno y publica el nombre solicitado para el archivo de salida solo después de que el codificador y el contenedor terminen correctamente. Se guarda un registro de conversión junto al archivo de salida.

Cuando la aceleración NVIDIA está disponible, la conversión usa NVDEC, CUDA, RVM mediante ONNX Runtime CUDA EP y NVENC cuando corresponde. En caso contrario, utiliza decodificación por software de FFmpeg, el modelo RVM FP32 mediante ONNX Runtime CPU EP y codificación por software.

## Reproductor de vídeo integrado

**Video Player...** abre un vídeo local independientemente de la selección DLNA.

Controles incluidos:

- Reproducir/Pausa
- Barra de búsqueda
- Tiempo actual y total
- Alternar pantalla completa
- Visualización de la mitad izquierda de SBS
- Espacio: Reproducir/Pausa
- Flecha izquierda/derecha: Retroceder o avanzar cinco segundos
- Escape: Salir de pantalla completa o cerrar el reproductor

El reproductor intenta usar NVIDIA NVDEC con los códecs compatibles y recurre a la decodificación por software cuando es necesario.

## Tratamiento de audio

- El modo `OFF` envía el archivo original sin modificar; el reproductor receptor gestiona su códec de audio.
- Las salidas Alpha Packed y Chroma Key en MPEG-TS usan audio AAC.
- La salida Alpha WebM VP9 usa audio Opus.
- Otros formatos de audio de origen decodificables por FFmpeg pueden transcodificarse cuando sea necesario.
- La entrada multicanal se mezcla a estéreo; la entrada mono permanece en mono.

## Compilación desde el código fuente

### Requisitos

- Windows 10 o Windows 11, de 64 bits
- Visual Studio 2022 con **Desarrollo para el escritorio con C++**
- CMake 3.24 o posterior
- NVIDIA CUDA Toolkit 12.8
- Acceso a Internet durante la descarga de las dependencias de compilación

### Compilación

Abra un símbolo del sistema en la raíz del repositorio y ejecute:

```bat
setup_dependencies.bat
build.bat
```

`setup_dependencies.bat` descarga las dependencias de desarrollo fijadas y los modelos ONNX oficiales de RVM. `build.bat` configura y compila el proyecto con CMake y NMake.

Los archivos generados se colocan en:

```text
build_cuda_ep\bin\r800zz_dlna_server.exe
build_cuda_ep\bin\r800zz_ai_worker.exe
build_cuda_ep\bin\rvm_mobilenetv3_fp16.onnx
build_cuda_ep\bin\rvm_mobilenetv3_fp32.onnx
```

La compilación actual fija el paquete compartido de FFmpeg en `N-124714-g49a77d37be`. No sustituya el paquete fijado por una compilación «latest» variable sin comprobar sus requisitos del controlador NVIDIA y de la API NVENC.

## Implementación

La ruta NVIDIA en tiempo real es:

```text
FFmpeg NVDEC
    -> preprocesamiento CUDA
    -> ONNX Runtime CUDA EP / RVM
    -> empaquetado alfa o composición de croma mediante CUDA
    -> FFmpeg NVENC
    -> MPEG-TS / DLNA
```

La ruta de alfa WebM auténtico prepara datos YUVA420P en CUDA, los transfiere a fotogramas de CPU reutilizables y usa `libvpx-vp9`, ya que las GPU NVIDIA no proporcionan codificación VP9 por hardware.

FFmpeg se utiliza mediante sus bibliotecas de C y DLL compartidas:

- `avformat`
- `avcodec`
- `avutil`
- `swresample`
- `swscale`

## Limitaciones conocidas

- Actualmente, el flujo de IA en tiempo real requiere una GPU NVIDIA.
- La aceleración mediante GPU AMD e Intel no está implementada.
- La codificación alfa WebM VP9 exige mucho a la CPU y puede no mantener la velocidad de fotogramas del vídeo de origen.
- La compatibilidad con alfa y croma varía entre los reproductores DLNA/XR.
- Actualmente, la aplicación está destinada a Windows x64.

## Componentes de terceros

Este proyecto utiliza:

- [Robust Video Matting](https://github.com/PeterL1n/RobustVideoMatting)
- [FFmpeg](https://ffmpeg.org/)
- [ONNX Runtime](https://github.com/microsoft/onnxruntime)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [NVIDIA CUDA Toolkit](https://developer.nvidia.com/cuda-toolkit)
- [NVIDIA cuDNN](https://developer.nvidia.com/cudnn)

Cada componente de terceros sigue sujeto a sus propios términos de licencia y redistribución.

## Licencia

Este proyecto se distribuye bajo la **Licencia Pública General de GNU v3.0**. Consulte [LICENSE](LICENSE).

El modelo RVM y los componentes derivados de RVM también están sujetos a la licencia del proyecto original Robust Video Matting. Las distribuciones del código fuente y las versiones binarias deben conservar todos los avisos de copyright y licencia aplicables.

## Enlaces

- [vr180g.com](https://vr180g.com/)
- [R800ZZ en YouTube](https://www.youtube.com/@R800ZZ)
