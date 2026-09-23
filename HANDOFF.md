# Handoff: RelightFX — plugin de relight en tiempo real para After Effects

## Objetivo del proyecto

Plugin nativo de After Effects (C++, AE SDK) que reilumina line art de manga/anime
en tiempo real: calcula un normal map a partir del propio dibujo y lo sombrea
contra un rig de luz 3D movible, sin pintar nada a mano y sin depender de un
modelo de IA pre-entrenado. Inspirado en un plugin comercial ("Efecs Relight A7")
del que solo se tienen capturas/video, no el código.

Uso previsto: personal, para el usuario y amigos editores que hacen manga MMV
(video edits musicales). No es un producto comercial (por ahora), lo cual
importa para decisiones de licencia tomadas en el camino.

## ⚠️ ESTADO ACTUAL: BLOQUEADO, EN MEDIO DE UN DEBUG

**Lo último que se encontró (aún sin confirmar que funciona):** el plugin
cargaba y aplicaba pero no generaba ningún sombreado visible, renderizando
siempre en ~5-6ms (sospechosamente rápido para el pipeline real). Con logging
de diagnóstico se confirmó la causa exacta: `in_data->sequence_data` llega
`NULL` dentro de `PF_Cmd_RENDER`, a pesar de que `PF_Cmd_SEQUENCE_SETUP` lo
setea correctamente. La documentación del SDK (`AE_Effect.h`, comentario sobre
`PF_OutFlag2_SUPPORTS_THREADED_RENDERING`) confirma que esto es **comportamiento
esperado**: con ese flag activado, `sequence_data` es de solo lectura y
`NULL` en render, a menos que se agregue también
`PF_OutFlag2_MUTABLE_RENDER_SEQUENCE_DATA_SLOWER`.

Se aplicó el fix (agregar ese flag en `GlobalSetup()` y actualizar el PiPL a
`0x18000000`), se recompiló, **pero el build corregido nunca llegó a instalarse**
porque el archivo `.aex` estaba bloqueado por una instancia de AE corriendo
(`cp: Device or resource busy`). La última captura del usuario sigue mostrando
el build viejo sin el fix (confirmado por hash MD5 distinto entre
`build/RelightFX.aex` y el archivo instalado en la carpeta de Plug-ins).

**Primer paso al retomar: cerrar AE por completo, copiar
`build/RelightFX.aex` (recompilar si hace falta) a la carpeta de Plug-ins, y
probar de nuevo.** Es muy posible que esto ya resuelva el problema.

## Rutas clave

- Repo local: `C:\Users\PC\Music\Intento de Plugin After Effects\`
- Repo GitHub: `https://github.com/SonizBeibe/motor-relighting-ae` (rama `main`;
  la rama `review-jules-pr1` tiene el trabajo de Jules + mis fixes, sin mergear
  a `main` todavía)
- Código del plugin: `src/RelightFX/RelightFX.cpp` (y `.h`, `_Strings.cpp/h`)
- Proyecto VS: `src/RelightFX/Win/RelightFX.vcxproj` / `.sln`
- Especificación completa del algoritmo: `JULES_TASK.md` (en la raíz del repo)
- AE SDK (NO está en el repo, es propietario): descargado en
  `E:\Soniz\Downloads\AfterEffectsSDK_26.5_win\AfterEffectsSDK_26.5_win\Examples`
  — el `.vcxproj` referencia esto vía macro `AE_SDK_DIR`
- OpenCV 4.8.0 (NO está en el repo): instalado en `C:\opencv\build`
  (`OPENCV_DIR` macro en el `.vcxproj`); DLL relevante:
  `C:\opencv\build\x64\vc16\bin\opencv_world480.dll` (Release) /
  `opencv_world480d.dll` (Debug)
- Cascade de detección de cara: `src/RelightFX/lbpcascade_animeface.xml`
  (de `github.com/nagadomi/lbpcascade_animeface`, sin licencia formal pero
  usado libremente hace +10 años en el ecosistema de anime CV — ok para uso
  personal)
- After Effects instalado en:
  `E:\Program Files\after effects 2025\Adobe After Effects 2026\`
  - Ejecutable: `Support Files\AfterFX.exe`
  - Carpeta de plugins: `Support Files\Plug-ins\` (el usuario dio permiso de
    escritura sobre esta carpeta puntual vía `icacls` — no hace falta admin
    para copiar ahí)
- Comp de prueba en AE: `E:\Soniz\Downloads\Test plugin Yuka.aep`
- Imagen de prueba principal: `yuka.psd` (5106x3191), dentro de
  `E:\Soniz\Downloads\drive-download-20260920T051514Z-1-001\JUJUTSU KAISEN MÓDULO-20260920T043356Z-1-001\JUJUTSU KAISEN MÓDULO\`
  (hay 14 PSD más ahí, variedad de poses/personajes para probar generalización)
- Build de salida: `build/RelightFX.aex` (variable de entorno
  `AE_PLUGIN_BUILD_DIR` apunta acá al compilar)
- Log de diagnóstico (temporal, para debug): se escribe en
  `<carpeta del plugin>\relightfx_debug.log` — HAY QUE SACAR este logging una
  vez resuelto el bug actual (está marcado con comentario "Remove once
  resolved" en el código)

## Toolchain de compilación

- Visual Studio Community 2026 (v18), toolset `v145`, C++17
- MSBuild: `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe`
- Comando de build (Git Bash, `MSYS2_ARG_CONV_EXCL="*"` necesario para que no
  rompa los flags con `/`):
  ```bash
  export AE_PLUGIN_BUILD_DIR="C:\Users\PC\Music\Intento de Plugin After Effects\build"
  export MSYS2_ARG_CONV_EXCL="*"
  "$MSBUILD" "<ruta al .sln>" -p:Configuration=Release -p:Platform=x64 -nologo -v:minimal
  ```
- **Compilar siempre en Release, no Debug** — Debug depende del runtime de
  depuración de C++ (`ucrtbased.dll`, etc.) que no está disponible fuera de
  una sesión de Visual Studio, y causa el mismo error engañoso
  ("Couldn't find main entry point") que el bug de delay-loading.
- Instalar en AE: copiar `RelightFX.aex` + `opencv_world480.dll` (Release,
  SIN la "d") + `lbpcascade_animeface.xml` a `Support Files\Plug-ins\`.
  **AE tiene que estar completamente cerrado** para poder sobreescribir el
  `.aex` (si no, da "Device or resource busy").
- Después de instalar: reiniciar AE por completo (matar el proceso, no solo
  cerrar la ventana) para que relea el plugin.

## Cronología de bugs ya resueltos (para no repetirlos)

1. **`rc.light_color = params[...]->u.cd;` sin `.value`** — error de tipos
   (`PF_ColorDef` vs `PF_Pixel8`), no compilaba. Arreglado.
2. **`face_nz = w + 1.0f;`** en el elipsoide de cara — sumaba 1.0 de más,
   aplastando el degradado de la cara. Debía ser `w` solo. Arreglado.
3. **Bulto de nariz sin escalar** — se sumaba entero en vez de multiplicado
   por `0.55` como en el prototipo Python validado. Arreglado
   (`kNoseStrength = 0.55f`).
4. **Parámetro "Height Map" (capa opcional pintada a mano)** quedó en la UI
   pero dejó de usarse en `Render()` — regresión silenciosa. Re-conectado
   (`PF_CHECKOUT_PARAM`/`PF_CHECKIN_PARAM` sobre `RELIGHT_HEIGHTMAP`, pasado
   como `heightSrcP` a `ComputeNormalMapWithOpenCV`).
5. **Parámetro "Normal Strength"** se leía pero nunca se aplicaba en
   `CombineFunc8` — dead code. Ahora se aplica como multiplicador global
   sobre `dx_combined`/`dy_combined` dentro de `ComputeNormalMapWithOpenCV`.
6. **Ruta del cascade XML como string relativo pelado** (`"lbpcascade_animeface.xml"`)
   — no resolvía bien según el directorio de trabajo de AE. Arreglado con
   `GetPluginDirectory()` (usa `GetModuleHandleExA` + `GetModuleFileNameA`
   para ubicar la carpeta del propio `.aex`).
7. **Excepciones de OpenCV sin atrapar** — el `try/catch` en `EffectMain`
   solo atrapaba `PF_Err&`, no `std::exception`/`cv::Exception`. Se agregaron
   catches adicionales (aunque este NO resultó ser la causa del bug actual,
   sigue siendo buena práctica dejarlo).
8. **"Couldn't find main entry point for RelightFX.aex"** — causa real:
   `opencv_world480.dll` es una dependencia implícita (link normal), y el
   orden de búsqueda de DLLs de Windows busca junto al `.exe` HOST
   (`AfterFX.exe`), no junto al plugin que la necesita. Nunca se encontraba
   aunque estuviera al lado del `.aex`. **Fix: delay-loading** —
   `<DelayLoadDLLs>opencv_world480.dll</DelayLoadDLLs>` en el `.vcxproj` +
   link contra `delayimp.lib` + un hook (`__pfnDliFailureHook2`) que resuelve
   la ruta absoluta manualmente vía `GetPluginDirectory()` si la búsqueda
   normal falla. Confirmado con `dumpbin /dependents` que ahora aparece bajo
   "delay load dependencies", no como dependencia dura.
   - Diagnóstico de esto se hizo leyendo
     `%APPDATA%\Adobe\After Effects\26.2\Plugin Loading.log`, que mostró
     `"The library could not be loaded. The error code: 2685337601"` (0xA00F0001)
     — muy útil para el futuro, ese log dice la verdad cuando el diálogo de
     AE es genérico/engañoso.
9. **`sequence_data` NULL en render** (bug actual, fix aplicado pero NO
   probado todavía) — ver sección de arriba.

## El algoritmo (validado extensamente en Python antes de portar a C++)

Todo esto se probó primero con Python/numpy/scipy/OpenCV contra `yuka.psd` y
contra referencias visuales reales (capturas y video del plugin comercial)
ANTES de escribir C++. No hay que rediseñar el algoritmo, solo terminar de
portarlo correctamente:

1. **Segmentación**: threshold sobre luma con blur previo (sigma~2) para que
   el tramado/screentone del manga no se lea como miles de líneas de tinta
   falsas. `ink_mask` = líneas oscuras; `silhouette_mask` = alpha del
   personaje.
2. **Distance transform por región**: `cv::distanceTransform` (L2, mask 5)
   sobre el inverso de la máscara, normalizado **por cada componente conexa
   por separado** (no con un máximo global — si no, una región grande domina
   y las chicas quedan planas).
3. **Height shaping**: `pow(d, exponente)` (default ~0.6, pero validado con
   valores más bajos ~0.28 en las pruebas del usuario) + `GaussianBlur`
   generoso para que no se vea "faceteado"/low-poly.
4. **Dos capas independientes, no una sola altura combinada**:
   - **Coarse**: distance transform solo contra el contorno EXTERIOR de la
     silueta (ignora líneas internas) → un domo suave por figura (da forma
     redondeada a pelo/hombros/cuerpo entero).
   - **Fine**: distance transform contra las líneas de tinta internas
     (mechones, pliegues, detalle chico).
   - Sus gradientes (`dx`,`dy`) se combinan con **multiplicadores de
     intensidad independientes** (`coarseStrength`, `fineStrength`) ANTES de
     sumarse — una región de cientos de píxeles (la cabeza) tiene una
     pendiente por píxel muchísimo más chica que un mechón de 10px, así que
     necesitan escalas de fuerza muy distintas para verse "igual de
     redondeadas".
5. **Cara (elipsoide analítico, no distance transform)** — solo cuando el
   detector de cara encuentra algo:
   - `lbpcascade_animeface.xml` vía `cv::CascadeClassifier`. Ajustar
     `scaleFactor`/`minNeighbors` — un default muy grueso
     (`scaleFactor=1.05, minNeighbors=3`) NO detectaba caras que sí detecta
     `scaleFactor=1.02, minNeighbors=2`. Esperar que **20-30% de paneles
     reales de manga no tengan cara detectable** (poses de acción,
     personajes de espaldas, monstruos, cara fuera de cuadro) — es normal,
     el fallback a "solo domo" tiene que andar bien en esos casos.
   - Elipsoide: `w = sqrt(max(0, 1 - u² - v²))` con `(u,v)` normalizado al
     bounding box de la cara detectada. Da la normal analítica directamente,
     sin distance transform.
   - **Bulto de nariz**: segundo elipsoide más chico, centrado en
     `cx + 2%*ancho_cara`, `cy + 12%*alto_cara`, escalado por
     `kNoseStrength=0.55` antes de sumarse — produce el triángulo de luz que
     rompe la sombra cerca de la nariz/boca, un detalle que la referencia
     SIEMPRE tiene y que sin esto no aparece.
   - **Protección de ojos**: zona de falloff suave (NO un corte duro) en
     `cx ± 11.5%*ancho`, `cy - 2.5%*alto`, radio `~10%*ancho`, que en el
     shading final restaura el píxel original en vez de aplicar la sombra —
     así el blanco del ojo no se ensucia aunque esté del lado sombreado
     (convención real de shading anime, confirmada mirando referencias con
     lupa).
   - Blend entre elipsoide de cara y domo grueso: dominante adentro del
     óvalo, con feather entre 75%-115% del radio para que no se note costura
     en la mandíbula.
   - La capa "fine" SÍ debe aportar un poco dentro de la cara (peso bajo,
     ~10-15% de su peso normal) — sin esto no aparece la sombrita bajo la
     ceja que la referencia tiene; con peso igual al de afuera, se ve como
     una mancha incoherente (ya probado y rechazado).
6. **Shading (toon/cel-shading)**:
   - `dot = clamp(dot(N, L), 0, 1)`
   - `smoothstep(threshold-eps, threshold+eps, dot)` — `eps` chico = corte
     duro tipo manga blanco/negro; `eps` grande = degradado suave tipo anime
     a color. Tiene que ser ajustable (`Shadow Hardness` slider) porque las
     dos referencias reales que se vieron usaban estilos distintos.
   - Multiply para sombra, screen para luz de color, con las líneas de tinta
     originales siempre redibujadas encima sin verse afectadas por la luz.
   - Restaurar píxel original dentro de la zona de protección de ojos.

## Caché / rendimiento

- Los pasos 1-4 (todo lo pesado: segmentación, distance transform, Sobel,
  detección de cara) dependen SOLO del dibujo, no de dónde está la luz. Se
  cachean en `sequence_data` (ver bug #9 arriba — requiere
  `PF_OutFlag2_MUTABLE_RENDER_SEQUENCE_DATA_SLOWER`), invalidados por un hash
  del contenido de píxeles + parámetros (NUNCA por `current_time` — eso
  invalidaría en cada frame aunque el dibujo no cambie, matando el propósito
  de la caché; esto ya se corrigió una vez que Jules lo hizo mal).
- Solo el paso 5 (dot product + toon step) corre por frame sin caché — así
  mover la luz debería sentirse fluido una vez que la caché esté andando de
  verdad.
- Pendiente de medir: tiempo real de cómputo de los pasos 1-4 sobre la imagen
  de 5106x3191 dentro de AE (el standalone de prueba, ver abajo, tardó
  ~5.3 segundos en esa imagen a resolución completa — puede ser mucho para
  la primera vez que se aplica el efecto o cuando cambia el dibujo).

## Herramienta de debug standalone (útil para seguir iterando sin pasar por AE)

En `tools/cv_debug/cv_debug.cpp` hay un programa de consola que corre
exactamente el mismo pipeline de OpenCV (pasos 1-4, sin la parte de AE) sobre
una imagen cargada con `cv::imread`, con try/catch detallado por paso. Sirve
para reproducir bugs de la lógica de OpenCV en segundos en vez de tener que
pasar por AE cada vez. Compilar con:
```powershell
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "call `"$vcvars`" >nul && cl.exe /EHsc /std:c++17 /O2 /I C:\opencv\build\include tools\cv_debug\cv_debug.cpp /Fe:tools\cv_debug\cv_debug.exe /link /LIBPATH:C:\opencv\build\x64\vc16\lib opencv_world480.lib"
```
Correrlo (necesita `opencv_world480.dll` en el PATH):
```bash
export PATH="/c/opencv/build/x64/vc16/bin:$PATH"
tools/cv_debug/cv_debug.exe <ruta_imagen> <ruta_cascade>
```
Con esto se confirmó que la lógica de OpenCV en sí NO tira excepciones ni
falla sobre `yuka.psd` — el bug de "no hace nada" está en la integración con
AE (sequence_data), no en el algoritmo.

## Caminos ya explorados y descartados (no reabrir)

- **DeepNormals** (red neuronal específica para esto, paper de
  Disney Research/V-Sense 2018): calidad muy buena en el paper, pero
  bloqueado — el servidor de Trinity College Dublin que aloja los pesos
  pre-entrenados está roto (falla el handshake TLS con cualquier cliente:
  curl, Python, .NET, hasta el crawler de Wayback Machine). No hay mirrors en
  ningún lado (ni HuggingFace, Kaggle, GitHub Releases, ni el fork que portó
  el código a TF2). Además licencia no-comercial (aceptable para uso
  personal, pero a tener en cuenta si esto se vuelve un producto). Se mandó
  mail a los autores pidiendo el archivo, sin respuesta aún.
- **MiDaS / modelos genéricos de profundidad**: probado directamente,
  resultado inútil — un modelo entrenado con fotos no entiende line art,
  solo detecta la silueta general como un blob liso, peor que el Sobel
  simple.
- **Sobel puro sobre el dibujo, sin domo de silueta ni cara**: es lo que
  proponía Gemini como "solución 100% matemática sin ML" — YA SE PROBÓ, es
  literalmente el punto de partida de este proyecto, y el resultado es
  incoherente (mancha sin sentido anatómico) porque no tiene forma de saber
  dónde está la cara. El elipsoide de cara es lo que lo arregla, y
  `lbpcascade_animeface` NO es una red neuronal (es una cascada Haar/LBP
  clásica de 2011, determinista en runtime, sin dataset ni servidor) así que
  no entra en el mismo problema que DeepNormals.
- **Mapa de altura pintado a mano**: descartado porque el objetivo explícito
  es que sea 100% automático al aplicar el efecto, sin trabajo manual por
  ilustración.

## Qué falta (más allá de resolver el bug actual)

1. Confirmar que el fix de `sequence_data` realmente arregla el "no hace
   nada" (instalar el build ya compilado, no probado aún).
2. Sacar el logging de diagnóstico una vez confirmado.
3. Medir tiempos reales dentro de AE y ver si 5+ segundos de cómputo inicial
   es aceptable o si hace falta optimizar (paralelizar el distance transform
   a mano, o considerar GPU si sigue siendo muy lento).
4. Probar contra las otras 13 imágenes PSD de la carpeta de Jujutsu Kaisen
   (no solo `yuka.psd`) para confirmar que el detector de cara y el domo
   grueso generalizan bien a poses distintas.
5. Mergear la rama `review-jules-pr1` a `main` en GitHub una vez que ande.
6. Revisar si vale la pena explorar Ollama (mencionado por el usuario) para
   algo puntual — probablemente NO para generar el normal map completo
   (mismo problema de fondo que MiDaS: un modelo genérico no entiende
   line art), pero podría tener algún uso acotado a evaluar con cautela y
   sin repetir el mismo error de expectativas que con MiDaS/DeepNormals.
