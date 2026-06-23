// ============================================================
//  Captura INMP441 - cliente movil (Flutter / Dart)
//  Pestaña 1 - Captura: conecta al ESP32 por TCP, recibe
//    rafagas PCM de 24 bits CRUDO (enviadas como int32 LE,
//    4 bytes/muestra) y las guarda como WAV de 24 bits real.
//  Pestaña 2 - Grabaciones: lista los WAV guardados, permite
//    reproducirlos, borrarlos y compartirlos.
//
//  CAMBIO IMPORTANTE respecto a la version anterior:
//    - El ESP32 ya NO aplica AGC ni mediana: envia el dato
//      crudo de 24 bits (en un contenedor int32, 4 bytes LE).
//    - Esta app empaqueta esas muestras a PCM de 24 bits real
//      (3 bytes/muestra) al escribir el WAV.
//    - La ganancia/truncado a 16 bits para Edge Impulse, etc.
//      se decide despues, en PC, sobre este WAV "master".
//
//  Dependencias: path_provider, just_audio, share_plus
//  Permisos:     INTERNET (AndroidManifest.xml)
//
//  NOTA: just_audio reproduce WAV de 24 bits sin problema en
//  la mayoria de dispositivos Android/iOS modernos. Si en tu
//  dispositivo especifico da problemas de reproduccion, es
//  solo un tema de reproduccion, no afecta los datos guardados.
// ============================================================

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:just_audio/just_audio.dart';
import 'package:path_provider/path_provider.dart';

void main() => runApp(const CapturaApp());

class CapturaApp extends StatelessWidget {
  const CapturaApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Captura INMP441',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(colorSchemeSeed: Colors.teal, useMaterial3: true),
      home: const RootPage(),
    );
  }
}

// ============================================================
//  Directorio compartido entre las dos pestañas
// ============================================================
Future<Directory> getCapturasDir() async {
  Directory? base;
  try {
    base = await getExternalStorageDirectory();
  } catch (_) {}
  base ??= await getApplicationDocumentsDirectory();
  final dir = Directory('${base.path}/capturas');
  if (!await dir.exists()) await dir.create(recursive: true);
  return dir;
}

// ============================================================
//  Pagina raiz con BottomNavigationBar
// ============================================================
class RootPage extends StatefulWidget {
  const RootPage({super.key});

  @override
  State<RootPage> createState() => _RootPageState();
}

class _RootPageState extends State<RootPage> {
  int _idx = 0;

  // Clave para poder llamar a refreshList() desde fuera
  final _recKey = GlobalKey<_GrabacionesPageState>();

  void _onTabChanged(int i) {
    setState(() => _idx = i);
    // Al entrar a la pestaña de grabaciones, refresca la lista
    if (i == 1) _recKey.currentState?.refreshList();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: IndexedStack(
        index: _idx,
        children: [
          CapturaPage(
              onNewRecording: () => _recKey.currentState?.refreshList()),
          GrabacionesPage(key: _recKey),
        ],
      ),
      bottomNavigationBar: NavigationBar(
        selectedIndex: _idx,
        onDestinationSelected: _onTabChanged,
        destinations: const [
          NavigationDestination(icon: Icon(Icons.mic), label: 'Captura'),
          NavigationDestination(
              icon: Icon(Icons.library_music), label: 'Grabaciones'),
        ],
      ),
    );
  }
}

// ============================================================
//  Pestaña 1 - Captura
// ============================================================
class CapturaPage extends StatefulWidget {
  final VoidCallback? onNewRecording;
  const CapturaPage({super.key, this.onNewRecording});

  @override
  State<CapturaPage> createState() => _CapturaPageState();
}

class _CapturaPageState extends State<CapturaPage> {
  final ipCtrl = TextEditingController(text: '10.109.146.120');
  final portCtrl = TextEditingController(text: '8000');
  final srCtrl = TextEditingController(text: '8000');
  final idleCtrl = TextEditingController(text: '0.8');
  final maxdurCtrl = TextEditingController(text: '30');
  final baseCtrl = TextEditingController(text: 'captura');

  // Bytes por muestra TAL COMO LLEGAN del ESP32: int32 LE = 4 bytes.
  static const int kBytesPerSampleEntrada = 4;
  // Bytes por muestra AL GUARDAR en el WAV: PCM 24 bits real = 3 bytes.
  static const int kBytesPerSampleWav = 3;
  static const int kBitsWav = 24;

  Socket? _socket;
  StreamSubscription<Uint8List>? _sub;
  bool _connected = false;
  String _status = 'Desconectado';
  int _count = 0;

  BytesBuilder _buf = BytesBuilder();
  bool _recording = false;
  Timer? _idleTimer;
  int _sr = 8000;
  double _idle = 0.8;
  double _maxdur = 30;
  String _base = 'captura';
  Directory? _outDir;

  final List<String> _logLines = [];
  final ScrollController _logScroll = ScrollController();

  @override
  void dispose() {
    _idleTimer?.cancel();
    _sub?.cancel();
    _socket?.destroy();
    ipCtrl.dispose();
    portCtrl.dispose();
    srCtrl.dispose();
    idleCtrl.dispose();
    maxdurCtrl.dispose();
    baseCtrl.dispose();
    _logScroll.dispose();
    super.dispose();
  }

  // ---------------- Conexion ----------------
  Future<void> _connect() async {
    if (_connected) return;
    final ip = ipCtrl.text.trim();
    final port = int.tryParse(portCtrl.text.trim());
    _sr = int.tryParse(srCtrl.text.trim()) ?? 0;
    _idle = double.tryParse(idleCtrl.text.trim()) ?? 0;
    _maxdur = double.tryParse(maxdurCtrl.text.trim()) ?? 0;
    _base = baseCtrl.text.trim().isEmpty ? 'captura' : baseCtrl.text.trim();

    if (port == null || _sr <= 0 || _idle <= 0 || _maxdur <= 0) {
      _log('Parametros invalidos.');
      return;
    }
    _setStatus('Conectando...');
    try {
      _outDir = await getCapturasDir();
      _socket =
          await Socket.connect(ip, port, timeout: const Duration(seconds: 10));
    } catch (e) {
      _setStatus('Error de conexion');
      _log('No se pudo conectar a $ip:$port -> $e');
      _socket = null;
      return;
    }
    _connected = true;
    _buf = BytesBuilder();
    _recording = false;
    _setStatus('Conectado - esperando boton');
    _log(
        'Conectado a $ip:$port. Usa "Grabar (remoto)" o el pulsador del ESP32.');
    _log('Formato: PCM 24 bits crudo (sin AGC). Carpeta: ${_outDir!.path}');

    _sub = _socket!.listen(
      _onData,
      onError: (e) {
        _log('Error de socket: $e');
        _disconnect();
      },
      onDone: () {
        _log('El ESP32 cerro la conexion.');
        _disconnect();
      },
      cancelOnError: true,
    );
  }

  void _disconnect() {
    _idleTimer?.cancel();
    if (_recording) {
      final bytes = _buf.takeBytes();
      _recording = false;
      if (bytes.isNotEmpty) _saveWav(bytes);
    }
    _sub?.cancel();
    _sub = null;
    _socket?.destroy();
    _socket = null;
    if (_connected) {
      _connected = false;
      _setStatus('Desconectado');
    }
  }

  // ---------------- Comando remoto de grabacion ----------------
  // Envia un solo byte 'G' (0x47) al ESP32 por el mismo socket TCP para
  // iniciar la grabacion sin tocar el pulsador fisico, evitando que el
  // clic del boton quede registrado al inicio del audio.
  static const int kCmdGrabar = 0x47; // 'G'

  void _enviarComandoGrabar() {
    if (!_connected || _socket == null) {
      _log('No se puede grabar: no hay conexion activa.');
      return;
    }
    if (_recording) {
      _log('Ya hay una grabacion en curso.');
      return;
    }
    try {
      _socket!.add(<int>[kCmdGrabar]);
      _log('Comando de grabacion enviado.');
    } catch (e) {
      _log('Error enviando comando: $e');
    }
  }

  // ---------------- Recepcion ----------------
  void _onData(Uint8List data) {
    if (!_recording) {
      _recording = true;
      _buf = BytesBuilder();
      _setStatus('Grabando...');
    }
    _buf.add(data);
    _idleTimer?.cancel();
    _idleTimer = Timer(
      Duration(milliseconds: (_idle * 1000).round()),
      _finishRecording,
    );
    // Duracion en segundos = bytes_recibidos / (sr * bytes_por_muestra_entrada)
    if (_buf.length / (_sr * kBytesPerSampleEntrada) >= _maxdur) {
      _idleTimer?.cancel();
      _finishRecording();
    }
  }

  Future<void> _finishRecording() async {
    if (!_recording) return;
    _recording = false;
    final bytes = _buf.takeBytes();
    _buf = BytesBuilder();
    if (_connected) _setStatus('Conectado - esperando boton');
    if (bytes.isNotEmpty) await _saveWav(bytes);
  }

  // ---------------- Guardado ----------------
  Future<String> _nextPath() async {
    int i = 1;
    while (true) {
      final p = '${_outDir!.path}/${_base}_${i.toString().padLeft(3, '0')}.wav';
      if (!await File(p).exists()) return p;
      i++;
    }
  }

  /// Convierte las muestras int32 LE (4 bytes, valor de 24 bits ya
  /// sign-extended por el ESP32) a PCM de 24 bits real (3 bytes/muestra).
  /// Simplemente se descarta el 4to byte (extension de signo, redundante);
  /// los 3 bytes restantes YA forman un PCM de 24 bits little-endian valido.
  Uint8List _packTo24Bit(Uint8List int32Bytes) {
    final n = int32Bytes.length ~/ kBytesPerSampleEntrada;
    final out = Uint8List(n * kBytesPerSampleWav);
    for (int i = 0; i < n; i++) {
      final srcOff = i * kBytesPerSampleEntrada;
      final dstOff = i * kBytesPerSampleWav;
      out[dstOff] = int32Bytes[srcOff]; // byte 0 (LSB)
      out[dstOff + 1] = int32Bytes[srcOff + 1]; // byte 1
      out[dstOff + 2] = int32Bytes[srcOff + 2]; // byte 2 (MSB de 24 bits)
      // byte 3 (extension de signo) se descarta intencionalmente
    }
    return out;
  }

  Future<void> _saveWav(Uint8List rawInt32) async {
    int len = rawInt32.length;
    // Asegurar multiplo de 4 (un int32 completo por muestra)
    len -= len % kBytesPerSampleEntrada;
    final trimmed = rawInt32.sublist(0, len);

    final pcm24 = _packTo24Bit(trimmed);

    final path = await _nextPath();
    await File(path).writeAsBytes(_buildWav24(pcm24, _sr), flush: true);

    final n = pcm24.length ~/ kBytesPerSampleWav;
    _log('Guardado: ${path.split('/').last}  '
        '($n muestras, ${(n / _sr).toStringAsFixed(2)} s, 24 bits PCM)');
    if (mounted) setState(() => _count++);
    widget.onNewRecording?.call();
  }

  /// Construye un WAV PCM de 24 bits (formato estandar, compatible con
  /// Audacity, librosa, soundfile, Edge Impulse, etc.)
  Uint8List _buildWav24(Uint8List pcm, int sampleRate) {
    const channels = 1;
    const bits = kBitsWav; // 24
    final byteRate = sampleRate * channels * bits ~/ 8;
    final blockAlign = channels * bits ~/ 8; // 3
    final h = BytesBuilder();
    void str(String s) => h.add(s.codeUnits);
    void u32(int v) => h.add(
        (ByteData(4)..setUint32(0, v, Endian.little)).buffer.asUint8List());
    void u16(int v) => h.add(
        (ByteData(2)..setUint16(0, v, Endian.little)).buffer.asUint8List());
    str('RIFF');
    u32(36 + pcm.length);
    str('WAVE');
    str('fmt ');
    u32(16);
    u16(1); // PCM entero
    u16(channels);
    u32(sampleRate);
    u32(byteRate);
    u16(blockAlign);
    u16(bits);
    str('data');
    u32(pcm.length);
    final out = BytesBuilder();
    out.add(h.takeBytes());
    out.add(pcm);
    return out.takeBytes();
  }

  // ---------------- UI helpers ----------------
  void _setStatus(String s) {
    if (mounted) setState(() => _status = s);
  }

  void _log(String msg) {
    final now = DateTime.now();
    String two(int v) => v.toString().padLeft(2, '0');
    final ts = '${two(now.hour)}:${two(now.minute)}:${two(now.second)}';
    if (mounted) {
      setState(() {
        _logLines.add('[$ts] $msg');
        if (_logLines.length > 200) _logLines.removeAt(0);
      });
    }
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_logScroll.hasClients)
        _logScroll.jumpTo(_logScroll.position.maxScrollExtent);
    });
  }

  Color _statusColor() {
    if (_status.startsWith('Grabando')) return Colors.orange;
    if (_status.startsWith('Conectado')) return Colors.green;
    if (_status.startsWith('Error') || _status == 'Desconectado')
      return Colors.red;
    return Colors.blueGrey;
  }

  Widget _field(String label, TextEditingController c, {bool number = false}) {
    return TextField(
      controller: c,
      keyboardType: number
          ? const TextInputType.numberWithOptions(decimal: true)
          : TextInputType.text,
      enabled: !_connected,
      decoration: InputDecoration(
          labelText: label, isDense: true, border: const OutlineInputBorder()),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Captura INMP441')),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Card(
              child: Padding(
                padding: const EdgeInsets.all(12),
                child: Column(children: [
                  _field('IP del ESP32', ipCtrl),
                  const SizedBox(height: 10),
                  Row(children: [
                    Expanded(child: _field('Puerto', portCtrl, number: true)),
                    const SizedBox(width: 10),
                    Expanded(
                        child:
                            _field('Sample rate (Hz)', srCtrl, number: true)),
                  ]),
                  const SizedBox(height: 10),
                  Row(children: [
                    Expanded(
                        child:
                            _field('Silencio fin (s)', idleCtrl, number: true)),
                    const SizedBox(width: 10),
                    Expanded(
                        child: _field('Duracion max (s)', maxdurCtrl,
                            number: true)),
                  ]),
                  const SizedBox(height: 10),
                  _field('Nombre base', baseCtrl),
                ]),
              ),
            ),
            const SizedBox(height: 8),
            Row(children: [
              Expanded(
                child: FilledButton.icon(
                  onPressed: _connected ? null : _connect,
                  icon: const Icon(Icons.link),
                  label: const Text('Conectar'),
                ),
              ),
              const SizedBox(width: 10),
              Expanded(
                child: OutlinedButton.icon(
                  onPressed: _connected ? _disconnect : null,
                  icon: const Icon(Icons.link_off),
                  label: const Text('Desconectar'),
                ),
              ),
            ]),
            const SizedBox(height: 12),
            FilledButton.icon(
              onPressed:
                  (_connected && !_recording) ? _enviarComandoGrabar : null,
              icon: const Icon(Icons.fiber_manual_record),
              label: const Text('Grabar (remoto)'),
              style: FilledButton.styleFrom(
                backgroundColor: Colors.redAccent,
                minimumSize: const Size.fromHeight(48),
              ),
            ),
            const SizedBox(height: 4),
            const Text(
              'Usa este boton en vez del pulsador fisico para evitar que '
              'el clic quede grabado al inicio del audio.',
              style: TextStyle(color: Colors.grey, fontSize: 12),
            ),
            const SizedBox(height: 12),
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                Row(children: [
                  const Text('Estado: '),
                  Text(_status,
                      style: TextStyle(
                          fontWeight: FontWeight.bold, color: _statusColor())),
                ]),
                Text('Grabaciones: $_count'),
              ],
            ),
            const SizedBox(height: 6),
            const Text(
              'Audio crudo de 24 bits (sin AGC). La ganancia/truncado a 16 '
              'bits se aplica despues en PC, sobre este WAV master.',
              style: TextStyle(color: Colors.grey, fontSize: 12),
            ),
            const SizedBox(height: 12),
            const Align(
              alignment: Alignment.centerLeft,
              child: Text('Registro',
                  style: TextStyle(fontWeight: FontWeight.bold)),
            ),
            const SizedBox(height: 4),
            Container(
              height: 220,
              padding: const EdgeInsets.all(8),
              decoration: BoxDecoration(
                border: Border.all(color: Colors.grey),
                borderRadius: BorderRadius.circular(6),
              ),
              child: ListView.builder(
                controller: _logScroll,
                itemCount: _logLines.length,
                itemBuilder: (_, i) => Text(_logLines[i],
                    style:
                        const TextStyle(fontFamily: 'monospace', fontSize: 12)),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

// ============================================================
//  Pestaña 2 - Grabaciones
// ============================================================
class GrabacionesPage extends StatefulWidget {
  const GrabacionesPage({super.key});

  @override
  State<GrabacionesPage> createState() => _GrabacionesPageState();
}

class _GrabacionesPageState extends State<GrabacionesPage> {
  List<File> _files = [];
  final AudioPlayer _player = AudioPlayer();
  String? _playingPath; // ruta del archivo en reproduccion
  bool _isPlaying = false;
  Duration _position = Duration.zero;
  Duration _duration = Duration.zero;

  @override
  void initState() {
    super.initState();
    _player.playerStateStream.listen((state) {
      if (!mounted) return;
      setState(() => _isPlaying = state.playing);
      if (state.processingState == ProcessingState.completed) {
        setState(() {
          _isPlaying = false;
          _position = Duration.zero;
        });
      }
    });
    _player.positionStream.listen((p) {
      if (mounted) setState(() => _position = p);
    });
    _player.durationStream.listen((d) {
      if (mounted) setState(() => _duration = d ?? Duration.zero);
    });
    refreshList();
  }

  @override
  void dispose() {
    _player.dispose();
    super.dispose();
  }

  Future<void> refreshList() async {
    final dir = await getCapturasDir();
    final all = dir
        .listSync()
        .whereType<File>()
        .where((f) => f.path.endsWith('.wav'))
        .toList()
      ..sort((a, b) => b.path.compareTo(a.path)); // mas reciente primero
    if (mounted) setState(() => _files = all);
  }

  // ---------------- Reproduccion ----------------
  Future<void> _togglePlay(File file) async {
    if (_playingPath == file.path && _isPlaying) {
      await _player.pause();
      return;
    }
    if (_playingPath == file.path && !_isPlaying) {
      await _player.play();
      return;
    }
    // nuevo archivo
    await _player.stop();
    setState(() {
      _playingPath = file.path;
      _position = Duration.zero;
      _duration = Duration.zero;
    });
    await _player.setFilePath(file.path);
    await _player.play();
  }

  Future<void> _stopPlay() async {
    await _player.stop();
    setState(() {
      _playingPath = null;
      _isPlaying = false;
      _position = Duration.zero;
    });
  }

  // ---------------- Borrado ----------------
  Future<void> _delete(File file) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (_) => AlertDialog(
        title: const Text('Borrar grabacion'),
        content: Text('¿Borrar "${file.path.split('/').last}"?'),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(context, false),
              child: const Text('Cancelar')),
          FilledButton(
              onPressed: () => Navigator.pop(context, true),
              child: const Text('Borrar')),
        ],
      ),
    );
    if (ok != true) return;
    if (_playingPath == file.path) await _stopPlay();
    await file.delete();
    refreshList();
  }

  // ---------------- Compartir ----------------
  Future<void> _share(File file) async {
    // Compartir via Intent de Android sin plugin externo
    const platform = MethodChannel('captura_inmp441/share');
    try {
      await platform.invokeMethod('shareFile', {'path': file.path});
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('No se pudo compartir: $e')),
        );
      }
    }
  }

  // ---------------- Formato ----------------
  String _fmt(Duration d) {
    final m = d.inMinutes.remainder(60).toString().padLeft(2, '0');
    final s = d.inSeconds.remainder(60).toString().padLeft(2, '0');
    return '$m:$s';
  }

  String _fileSize(File f) {
    final bytes = f.lengthSync();
    if (bytes < 1024) return '${bytes} B';
    if (bytes < 1048576) return '${(bytes / 1024).toStringAsFixed(1)} KB';
    return '${(bytes / 1048576).toStringAsFixed(1)} MB';
  }

  // ---------------- UI ----------------
  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text('Grabaciones (${_files.length})'),
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            tooltip: 'Actualizar lista',
            onPressed: refreshList,
          ),
        ],
      ),
      body: _files.isEmpty
          ? const Center(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Icon(Icons.mic_off, size: 64, color: Colors.grey),
                  SizedBox(height: 12),
                  Text('No hay grabaciones todavia.',
                      style: TextStyle(color: Colors.grey)),
                ],
              ),
            )
          : ListView.separated(
              padding: const EdgeInsets.symmetric(vertical: 8),
              itemCount: _files.length,
              separatorBuilder: (_, __) => const Divider(height: 1),
              itemBuilder: (_, i) => _buildTile(_files[i]),
            ),
    );
  }

  Widget _buildTile(File file) {
    final name = file.path.split('/').last;
    final isThis = _playingPath == file.path;
    final colorScheme = Theme.of(context).colorScheme;

    return Column(
      children: [
        ListTile(
          leading: CircleAvatar(
            backgroundColor: isThis
                ? colorScheme.primaryContainer
                : colorScheme.surfaceContainerHighest,
            child: Icon(
              isThis && _isPlaying ? Icons.pause : Icons.play_arrow,
              color:
                  isThis ? colorScheme.primary : colorScheme.onSurfaceVariant,
            ),
          ),
          title:
              Text(name, style: const TextStyle(fontWeight: FontWeight.w500)),
          subtitle: Text(
            isThis
                ? '${_fmt(_position)} / ${_fmt(_duration)}'
                : _fileSize(file),
            style: TextStyle(
              fontSize: 12,
              color: isThis ? colorScheme.primary : null,
            ),
          ),
          onTap: () => _togglePlay(file),
          trailing: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              IconButton(
                icon: const Icon(Icons.share),
                tooltip: 'Compartir',
                onPressed: () => _share(file),
              ),
              IconButton(
                icon: const Icon(Icons.delete_outline),
                tooltip: 'Borrar',
                color: Colors.red,
                onPressed: () => _delete(file),
              ),
            ],
          ),
        ),
        // Barra de progreso solo para el archivo activo
        if (isThis)
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16),
            child: SliderTheme(
              data: SliderTheme.of(context).copyWith(
                  trackHeight: 2,
                  thumbShape:
                      const RoundSliderThumbShape(enabledThumbRadius: 6)),
              child: Slider(
                min: 0,
                max: _duration.inMilliseconds
                    .toDouble()
                    .clamp(1, double.infinity),
                value: _position.inMilliseconds
                    .toDouble()
                    .clamp(0, _duration.inMilliseconds.toDouble()),
                onChanged: (v) =>
                    _player.seek(Duration(milliseconds: v.toInt())),
              ),
            ),
          ),
      ],
    );
  }
}
