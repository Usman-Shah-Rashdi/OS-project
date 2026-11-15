// MiniFS Explorer: a small web interface for MiniFS.
//
//   node gui/server.js        then open http://localhost:3000
//
// The server has no dependencies. For every request it runs the MiniFS
// program (build/minifs.exe) in --quiet mode, sends it one command, and
// returns the result, so all file-system logic stays in the C++ code.
//
// Disks: the page lists every MiniFS disk image found in the project folder
// (virtual_disk.bin) and in the disks/ folder, where new disks are created.
//
// Settings (environment variables):
//   PORT         web port (default 3000)
//   MINIFS_EXE   path to the MiniFS program (default: build/minifs[.exe])
//   MINIFS_DISK  disk opened at start (default: virtual_disk.bin in the project folder)
//   MINIFS_DISKS_DIR  where new disks are created (default: the disks/ folder)

'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const PROJECT = path.resolve(__dirname, '..');
const PUBLIC = path.join(__dirname, 'public');
const PORT = Number(process.env.PORT) || 3000;
const DEFAULT_DISK = path.resolve(process.env.MINIFS_DISK || path.join(PROJECT, 'virtual_disk.bin'));
const DISKS_FOLDER = path.resolve(process.env.MINIFS_DISKS_DIR || path.join(PROJECT, 'disks'));
const DISK_FOLDERS = [...new Set([path.dirname(DEFAULT_DISK), DISKS_FOLDER])];
const EXE = process.env.MINIFS_EXE || findProgram();

let currentDisk = DEFAULT_DISK;

function findProgram() {
  const names = process.platform === 'win32' ? ['minifs.exe'] : ['minifs'];
  const folders = ['build', 'build/Debug', 'build/Release'];
  for (const folder of folders) {
    for (const name of names) {
      const candidate = path.join(PROJECT, folder, name);
      if (fs.existsSync(candidate)) return candidate;
    }
  }
  return null;
}

// Quote one argument for the MiniFS command line (see tokenize() in cli.cpp).
function quote(text) {
  return '"' + String(text)
    .replace(/\\/g, '\\\\')
    .replace(/"/g, '\\"')
    .replace(/\r\n?/g, '\n')
    .replace(/\n/g, '\\n')
    .replace(/\t/g, '\\t') + '"';
}

// Everything that touches a disk file runs one job at a time, so two requests
// never use a disk at once (and a disk is never deleted while in use).
let queue = Promise.resolve();
function exclusive(work) {
  const job = queue.then(work);
  queue = job.catch(() => {});
  return job;
}

// Runs MiniFS commands on a disk. Resolves to { stdout: Buffer, error: string }.
function runMiniFS(commands, disk = currentDisk) {
  return exclusive(() => new Promise((resolve, reject) => {
    if (!EXE) {
      reject(new Error('MiniFS program not found. Build it first: cmake --build build'));
      return;
    }
    const child = spawn(EXE, ['--disk', disk, '--quiet'], {
      cwd: path.dirname(disk),
      windowsHide: true,
    });
    const out = [];
    let err = '';
    child.stdout.on('data', (chunk) => out.push(chunk));
    child.stderr.on('data', (chunk) => { err += chunk; });
    child.on('error', reject);
    child.on('close', () => {
      const error = err.trim().split(/\r?\n/).filter(Boolean)
        .map((line) => line.replace(/^Error:\s*/, '')).join('\n');
      resolve({ stdout: Buffer.concat(out), error });
    });
    child.stdin.end(commands.join('\n') + '\n');
  }));
}

// ---------------------------------------------------------------------------
// Disk images
// ---------------------------------------------------------------------------

const MAGIC = 0x53464e4d;  // "MNFS"
const FS_VERSION = 2;      // kFsVersion in fs_types.hpp
const MODES = ['contiguous', 'linked', 'indexed'];

const diskId = (file) => path.relative(PROJECT, file).split(path.sep).join('/');
const diskName = (file) => path.basename(file, '.bin');

// Reads the superblock (see struct Superblock in fs_types.hpp). Returns null
// if the file is not a MiniFS disk.
function readDiskInfo(file) {
  let stat;
  const header = Buffer.alloc(88);
  try {
    stat = fs.statSync(file);
    if (!stat.isFile() || stat.size < 88) return null;
    const fd = fs.openSync(file, 'r');
    try {
      fs.readSync(fd, header, 0, 88, 0);
    } finally {
      fs.closeSync(fd);
    }
  } catch {
    return null;
  }
  if (header.readUInt32LE(0) !== MAGIC) return null;
  const blockSize = header.readUInt32LE(8);
  const dataBlocks = header.readUInt32LE(44);
  const freeBlocks = header.readUInt32LE(48);
  return {
    id: diskId(file),
    name: diskName(file),
    path: file,
    version: header.readUInt32LE(4),
    supported: header.readUInt32LE(4) === FS_VERSION,
    fileSize: stat.size,
    dataBytes: dataBlocks * blockSize,
    freeBytes: freeBlocks * blockSize,
    mode: MODES[header.readUInt32LE(60)] || 'unknown',
    createdAt: Number(header.readBigInt64LE(72)),
  };
}

function listDisks() {
  const disks = [];
  const seen = new Set();
  for (const folder of DISK_FOLDERS) {
    let names = [];
    try {
      names = fs.readdirSync(folder);
    } catch {
      continue;
    }
    for (const name of names.filter((n) => n.toLowerCase().endsWith('.bin')).sort()) {
      const file = path.join(folder, name);
      if (seen.has(file)) continue;
      seen.add(file);
      const info = readDiskInfo(file);
      if (info) disks.push({ ...info, current: file === currentDisk });
    }
  }
  return disks;
}

// Only disks from the list can be opened or deleted, never arbitrary paths.
function findDisk(id) {
  return listDisks().find((disk) => disk.id === id) || null;
}

function validDiskName(name) {
  return typeof name === 'string' && /^[A-Za-z0-9][A-Za-z0-9 _-]{0,39}$/.test(name.trim());
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

const ACTIONS = {
  createdir: 1, create: 1, write: 2, delete: 1, deletedir: 1,
  rename: 2, copy: 2, move: 2,
};

function formatCommand(body) {
  const mb = Number(body.mb);
  if (!Number.isInteger(mb) || mb < 1 || mb > 1024 || !MODES.includes(body.alloc)) {
    throw new UserError('Choose a size of 1-1024 MB and an allocation method.');
  }
  return `format ${mb} --alloc=${body.alloc}`;
}

class UserError extends Error {}

async function handleApi(req, res, url) {
  const route = `${req.method} ${url.pathname}`;
  const disk = { id: diskId(currentDisk), name: diskName(currentDisk), path: currentDisk };

  if (route === 'GET /api/state') {
    const { stdout, error } = await runMiniFS(['export -']);
    if (error || stdout.length === 0) {
      return sendJson(res, 200, { formatted: false, exists: fs.existsSync(currentDisk), message: error, disk });
    }
    const state = JSON.parse(stdout.toString('utf8'));
    delete state.owners;  // per-block owners: large and not needed by the page
    // Changes on every run (each request mounts the disk), so leaving it in
    // would make the page redraw even when no file changed.
    delete state.superblock.mounted_at;
    return sendJson(res, 200, { formatted: true, disk, ...state });
  }

  if (route === 'GET /api/read') {
    const file = url.searchParams.get('path') || '';
    const { stdout, error } = await runMiniFS([`read ${quote(file)}`]);
    if (error) return sendJson(res, 400, { error });
    return sendJson(res, 200, { content: stdout.toString('utf8') });
  }

  if (route === 'POST /api/action') {
    const body = await readBody(req);
    const { action, args = [] } = body;
    let command;
    if (action === 'format') {
      command = formatCommand(body);
    } else if (action === 'chmod') {
      if (!/^[0-7]{3}$/.test(String(args[0])) || typeof args[1] !== 'string') {
        throw new UserError('Permissions must be three octal digits, like 644.');
      }
      command = `chmod ${args[0]} ${quote(args[1])}`;
    } else if (ACTIONS[action] !== undefined) {
      if (!Array.isArray(args) || args.length !== ACTIONS[action] ||
          !args.every((arg) => typeof arg === 'string')) {
        throw new UserError(`Wrong arguments for ${action}.`);
      }
      command = [action, ...args.map(quote)].join(' ');
    } else {
      throw new UserError(`Unknown action: ${action}`);
    }
    const { error } = await runMiniFS([command]);
    if (error) return sendJson(res, 400, { error });
    return sendJson(res, 200, { ok: true });
  }

  if (route === 'GET /api/disks') {
    return sendJson(res, 200, { disks: listDisks(), current: disk });
  }

  if (route === 'POST /api/disks/open') {
    const { id } = await readBody(req);
    const found = findDisk(id);
    if (!found) throw new UserError('That disk no longer exists.');
    currentDisk = found.path;
    return sendJson(res, 200, { ok: true });
  }

  if (route === 'POST /api/disks/create') {
    const body = await readBody(req);
    if (!validDiskName(body.name)) {
      throw new UserError('Disk names use letters, digits, spaces, - and _ (up to 40 characters).');
    }
    const command = formatCommand(body);
    const file = path.join(DISKS_FOLDER, `${body.name.trim()}.bin`);
    if (fs.existsSync(file)) throw new UserError(`A disk named "${body.name.trim()}" already exists.`);
    fs.mkdirSync(DISKS_FOLDER, { recursive: true });
    const { error } = await runMiniFS([command], file);
    if (error) return sendJson(res, 400, { error });
    currentDisk = file;
    return sendJson(res, 200, { ok: true });
  }

  if (route === 'POST /api/disks/delete') {
    const { id } = await readBody(req);
    const found = findDisk(id);
    if (!found) throw new UserError('That disk no longer exists.');
    await exclusive(async () => {
      try {
        fs.unlinkSync(found.path);
      } catch (error) {
        if (error.code === 'EBUSY' || error.code === 'EPERM') {
          throw new UserError(`"${found.name}" is open in another program (probably the MiniFS terminal). Close it and try again.`);
        }
        throw error;
      }
    });
    if (found.path === currentDisk) {
      // Open another disk if there is one, otherwise the default location.
      const remaining = listDisks();
      currentDisk = remaining.length ? remaining[0].path : DEFAULT_DISK;
    }
    return sendJson(res, 200, { ok: true });
  }

  return sendJson(res, 404, { error: 'Not found' });
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks = [];
    req.on('data', (chunk) => {
      size += chunk.length;
      if (size > 16 * 1024 * 1024) {
        reject(new UserError('Request too large.'));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => {
      try {
        resolve(JSON.parse(Buffer.concat(chunks).toString('utf8') || '{}'));
      } catch {
        reject(new UserError('Request body is not valid JSON.'));
      }
    });
    req.on('error', reject);
  });
}

function sendJson(res, status, data) {
  res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' });
  res.end(JSON.stringify(data));
}

// ---------------------------------------------------------------------------
// Static files
// ---------------------------------------------------------------------------

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.svg': 'image/svg+xml',
};

function serveStatic(res, url) {
  const relative = url.pathname === '/' ? 'index.html' : decodeURIComponent(url.pathname.slice(1));
  const file = path.normalize(path.join(PUBLIC, relative));
  if (!file.startsWith(PUBLIC + path.sep)) {
    res.writeHead(403).end();
    return;
  }
  fs.readFile(file, (error, data) => {
    if (error) {
      res.writeHead(404, { 'Content-Type': 'text/plain' }).end('Not found');
      return;
    }
    res.writeHead(200, { 'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream' });
    res.end(data);
  });
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
  if (!url.pathname.startsWith('/api/')) {
    serveStatic(res, url);
    return;
  }
  try {
    await handleApi(req, res, url);
  } catch (error) {
    sendJson(res, error instanceof UserError ? 400 : 500, { error: error.message });
  }
});

server.listen(PORT, '127.0.0.1', () => {
  console.log(`MiniFS Explorer running at http://localhost:${PORT}`);
  console.log(`  program: ${EXE || 'NOT FOUND - build MiniFS first (cmake --build build)'}`);
  console.log(`  disks:   ${DISK_FOLDERS.join(' and ')}`);
  console.log('Press Ctrl+C to stop.');
});
