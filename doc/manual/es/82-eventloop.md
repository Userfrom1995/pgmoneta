\newpage

## Bucle de Eventos (Event Loop)

Aquí encontrará una descripción concisa orientada a desarrolladores sobre el funcionamiento del bucle de eventos implementado en `ev.c` y `ev.h`. Esto incluye la arquitectura, backends, mecánica de observadores (watchers), ciclo de vida, separación de roles, flujo de callbacks y mejoras planificadas.

**Arquitectura de Alto Nivel**

El bucle de eventos permanece en un ciclo continuo de espera, monitoreando fuentes de eventos (disponibilidad de E/S, expiración de temporizadores o señales entregadas), despachando el manejador apropiado cuando ocurre un evento y llamando a una función de retorno (callback) registrada por el usuario.

* **Backends**: Abstraen el mecanismo de espera o E/S asíncrona específico del sistema operativo. Los backends soportados incluyen `io_uring`, `epoll` y `kqueue`.
* **Watchers**: Encapsulan el interés en un tipo específico de evento (E/S, señal o periódico) y transportan el callback y el contexto requeridos.
* **Rutinas del ciclo de vida**: Administran la configuración, ejecución, interrupción y destrucción del bucle.
* **Aislamiento de procesos**: Cada proceso posee su propio bucle de eventos, garantizando una clara distinción entre el proceso principal (aceptación de conexiones y manejo de señales/temporizadores globales) y los procesos trabajadores (gestión de sesiones de clientes individuales).

**Configuración**

El backend del bucle de eventos se puede seleccionar en `pgmoneta.conf` dentro de la sección `[pgmoneta]` mediante el parámetro `ev_backend`:

```ini
ev_backend = auto
```

Opciones admitidas:

* `auto`: Selecciona automáticamente el backend predeterminado de la plataforma para el sistema operativo (predeterminado). En Linux, selecciona `epoll`. En sistemas BSD/macOS, selecciona `kqueue`. `io_uring` es un backend opcional que debe configurarse explícitamente con `ev_backend = io_uring`.
* `io_uring`: Interfaz de E/S asíncrona de Linux mediante colas de envío y finalización del kernel con `liburing >= 2.5`. Es un backend opcional; si el kernel no admite las operaciones requeridas, `pgmoneta` recurre a `epoll`.
* `epoll`: Mecanismo estándar de notificación de eventos de E/S de Linux mediante `epoll_create1(2)` y `epoll_wait(2)`.
* `kqueue`: Mecanismo escalable de notificación de eventos del kernel disponible en FreeBSD, OpenBSD y macOS mediante `kqueue(2)` y `kevent(2)`.

**Backends**

1. **io_uring (Linux liburing >= 2.5)**:
   * Proporciona anillos modernos de envío y finalización de E/S asíncrona a nivel de kernel (`ring_rcv` y `ring_snd`).
   * Elimina la sobrecarga de llamadas al sistema durante operaciones de red estables mediante anillos de búfer.
   * Las señales se gestionan mediante `sigaction()` de POSIX con banderas pendientes atómicas despachadas en cada iteración del bucle.
   * Los temporizadores periódicos utilizan entradas de tiempo del kernel (`IORING_OP_TIMEOUT`).

2. **epoll (Predeterminado en Linux)**:
   * Utiliza el monitoreo por flanco/nivel de `epoll(7)` en Linux.
   * Las señales se gestionan mediante `sigaction()` de POSIX con banderas pendientes atómicas despachadas en cada iteración del bucle.
   * Los temporizadores periódicos utilizan `timerfd_create(2)` y `timerfd_settime(2)` para tiempos de espera de resolución de milisegundos.

3. **kqueue (Predeterminado en BSD/macOS)**:
   * Mecanismo de filtrado y notificación nativo en sistemas BSD y macOS.
   * Soporta `EVFILT_READ` y `EVFILT_TIMER` en una cola del kernel; las señales se gestionan mediante `sigaction()` de POSIX con banderas pendientes atómicas despachadas en cada iteración del bucle.

**Estructuras de Datos**

* `struct event_loop` centraliza todo el estado del bucle:
  * `running`: Bandera booleana atómica que controla la ejecución del bucle.
  * `forked`: Bandera booleana atómica que indica si el bucle fue heredado tras un `fork()`.
  * `owner_pid`: PID del proceso que creó el bucle.
  * `sigset`: Registra las señales interceptadas por el bucle.
  * `events`: Arreglo de punteros genéricos `event_watcher_t*` que representan los observadores activos.
  * Descriptores de backend (`ring_rcv` / `ring_snd` para `io_uring`, `epollfd` para `epoll`, `kqueuefd` para `kqueue`).
  * `buffer`: Búfer de memoria intermedia o anillo de búferes utilizado para transferir datos eficientemente.

**Tipos de Observadores (Watchers) y Responsabilidades**

Cada observador incluye un encabezado común (`event_watcher_t`) con su tipo, lo que permite iterar sobre arreglos heterogéneos de observadores.

1. **Observadores de E/S (`struct io_watcher`)**:
   Monitorean descriptores de archivo para lectura o escritura.
   * *Principal (Main)*: Monitorea `listen_fd` para conexiones entrantes, invocando el callback de aceptación para inicializar nuevos sockets.
   * *Trabajador (Worker)*: Monitorea sockets de comunicación (`rcv_fd` y `snd_fd`) para procesar solicitudes y respuestas de clientes.
   * Rutinas auxiliares: `pgmoneta_event_accept_init`, `pgmoneta_event_worker_init`, `pgmoneta_io_start`, `pgmoneta_io_stop` y `pgmoneta_io_send`.

2. **Observadores de Señales (`struct signal_watcher`)**:
   Capturan señales POSIX (`SIGTERM`, `SIGINT`, `SIGHUP`, etc.) y las convierten en notificaciones del bucle de eventos.
   * Se gestionan mediante `sigaction()` de POSIX con banderas pendientes atómicas despachadas en cada iteración del bucle.
   * Inicializados con `pgmoneta_signal_init`, registrados con `pgmoneta_signal_start` y detenidos con `pgmoneta_signal_stop`.

3. **Observadores Periódicos (`struct periodic_watcher`)**:
   Ejecutan callbacks a intervalos fijos de milisegundos o como temporizadores de un solo disparo.
   * Utilizados para tareas en segundo plano como validación de retención, limpieza de caché de métricas de Prometheus y verificación de conexiones inactivas.
   * Emplea `timerfd` en Linux (para `epoll`), estructuras timespec del kernel (para `io_uring`) y `EVFILT_TIMER` (para `kqueue`).
   * Inicializados con `pgmoneta_periodic_init(watcher, cb, msec, repeat_ms)`, y controlados mediante `pgmoneta_periodic_start` y `pgmoneta_periodic_stop`.

**Ciclo de Vida del Bucle de Eventos**

1. **Inicialización (`pgmoneta_event_loop_init`)**:
   Asigna e inicializa la estructura `struct event_loop`, inspecciona `config->ev_backend`, detecta capacidades del sistema y configura los descriptores del backend (`io_uring`, `epoll` o `kqueue`).
2. **Ejecución (`pgmoneta_event_loop_run`)**:
   Ingresa al bucle de espera continuo. Aguarda eventos de los mecanismos del kernel, ejecuta callbacks registrados y comprueba la bandera atómica `running` en cada iteración.
3. **Interrupción (`pgmoneta_event_loop_break`)**:
   Establece la bandera `running` en `false` de forma atómica y despierta el ciclo de espera para que `pgmoneta_event_loop_run` termine limpiamente.
4. **Destrucción (`pgmoneta_event_loop_destroy`)**:
   Detiene todos los observadores registrados, cierra los descriptores de backend (`epollfd`, `kqueuefd` o anillos de `io_uring`), libera búferes y desaloca la estructura del bucle.
5. **Manejo de Bifurcación (`pgmoneta_event_loop_fork`)**:
   Invocado inmediatamente después de un `fork()` en los procesos hijos para cerrar descriptores heredados, máscaras de señales y manijas de backend del proceso padre, previniendo corrupción entre procesos.

**Observadores de E/S: Principal vs Trabajador**

Para simplificar el manejo de conexiones, `pgmoneta` utiliza un modelo de procesos donde procesos trabajadores atienden las interacciones con los clientes:

* **Proceso Principal**:
  1. Registra observadores de E/S en los sockets de escucha (`listen_fd`).
  2. Registra observadores de señales administrativas (`SIGTERM`, `SIGINT`, `SIGHUP`).
  3. Registra temporizadores periódicos para tareas de fondo.
  4. Al aceptar una conexión, bifurca (`fork`) un proceso trabajador.

* **Proceso Trabajador**:
  1. Llama a `pgmoneta_event_loop_fork` para reiniciar el estado heredado del bucle.
  2. Registra observadores de E/S sobre los descriptores de conexión del cliente (`rcv_fd`, `snd_fd`).
  3. Despacha mensajes entrantes del protocolo de gestión o respaldo a los manejadores correspondientes.

**Mejoras Planificadas**

Diversos mecanismos de optimización están contemplados como opciones de compilación para su futura evaluación:

* **Zero Copy** (`MSG_ZEROCOPY` vía io_uring): Reduce el uso de CPU evitando copias intermedias de búfer entre espacio de usuario y kernel.
* **Fast Poll** (`EPOLLET`): Modo de sondeo de epoll por flanco para escenarios de alto rendimiento.
* **Huge Pages** (`IORING_SETUP_NO_MMAP`): Utiliza asignaciones de páginas grandes para los anillos de búfer de `io_uring`.
* **Multishot Recv**: Una única entrada de sumisión (SQE) que entrega múltiples finalizaciones de recepción.
* **IOVecs**: Vectores de E/S dispersa/reunida para consolidar operaciones de red.
