/**
 * Tests for noSnapshot: 'external' mode.
 *
 * Verifies that:
 *   1. Two extensions with different source but the same WIT produce
 *      byte-identical shell.wasm files.
 *   2. At runtime, mounting different extension.js files produces
 *      different results.
 */
import { readFile, writeFile, mkdir, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { randomBytes } from 'node:crypto';
import { suite, test, expect } from 'vitest';

import { componentize } from '@bytecodealliance/componentize-js';
import { transpile } from '@bytecodealliance/jco';

const witWorld = `
package local:ext-test;

world the-world {
  export hello: func() -> string;
}
`;

const sourceA = `
export function hello() {
  return 'extension-A';
}
`;

const sourceB = `
export function hello() {
  return 'extension-B';
}
`;

suite('noSnapshot external', () => {
  test('shell.wasm is byte-identical for different sources with same WIT', async () => {
    const [resultA, resultB] = await Promise.all([
      componentize(sourceA, {
        witWorld,
        noSnapshot: 'external',
        disableFeatures: ['random', 'clocks', 'http', 'stdio'],
      }),
      componentize(sourceB, {
        witWorld,
        noSnapshot: 'external',
        disableFeatures: ['random', 'clocks', 'http', 'stdio'],
      }),
    ]);

    const a = Buffer.from(resultA.component);
    const b = Buffer.from(resultB.component);

    expect(a.length).toBe(b.length);
    expect(a.equals(b)).toBe(true);
  });

  test('runtime: different extension.js files produce different results', async () => {
    // Build the shared shell once
    const { component } = await componentize(sourceA, {
      witWorld,
      noSnapshot: 'external',
      disableFeatures: ['random', 'clocks', 'http', 'stdio'],
    });

    // Use test/output/ so the transpiled code can find node_modules
    const testDir = fileURLToPath(new URL('.', import.meta.url));

    // Create extension directories in a temp location
    const extBase = join(tmpdir(), `cjs-ext-test-${randomBytes(6).toString('hex')}`);
    const extDirA = join(extBase, 'a');
    const extDirB = join(extBase, 'b');
    await mkdir(extDirA, { recursive: true });
    await mkdir(extDirB, { recursive: true });
    await writeFile(join(extDirA, 'extension.js'), sourceA);
    await writeFile(join(extDirB, 'extension.js'), sourceB);

    // Transpile output goes inside project tree (for node_modules resolution)
    const outDirA = join(testDir, 'output', 'ext-shell-a');
    const outDirB = join(testDir, 'output', 'ext-shell-b');
    await mkdir(join(outDirA, 'interfaces'), { recursive: true });
    await mkdir(join(outDirB, 'interfaces'), { recursive: true });

    const wasiMap = {
      'wasi:cli-base/*': '@bytecodealliance/preview2-shim/cli-base#*',
      'wasi:clocks/*': '@bytecodealliance/preview2-shim/clocks#*',
      'wasi:filesystem/*': '@bytecodealliance/preview2-shim/filesystem#*',
      'wasi:http/*': '@bytecodealliance/preview2-shim/http#*',
      'wasi:io/*': '@bytecodealliance/preview2-shim/io#*',
      'wasi:logging/*': '@bytecodealliance/preview2-shim/logging#*',
      'wasi:poll/*': '@bytecodealliance/preview2-shim/poll#*',
      'wasi:random/*': '@bytecodealliance/preview2-shim/random#*',
      'wasi:sockets/*': '@bytecodealliance/preview2-shim/sockets#*',
    };

    try {
      // Transpile the same shell into two separate directories
      // (needed to defeat Node.js module cache)
      const { files } = await transpile(component, {
        name: 'ext-shell',
        map: wasiMap,
        wasiShim: true,
        validLiftingOptimization: false,
      });

      for (const [name, content] of Object.entries(files)) {
        await writeFile(join(outDirA, name), content);
        await writeFile(join(outDirB, name), content);
      }

      // Configure preview2-shim – it's a singleton shared across imports
      const shimFs = await import('@bytecodealliance/preview2-shim/filesystem');
      const shimCli = await import('@bytecodealliance/preview2-shim/cli');

      // --- Run with extension A ---
      shimFs._setPreopens({ '.': extDirA });
      shimCli._setEnv({});

      const modA = await import(join(outDirA, 'ext-shell.js'));
      expect(modA.hello()).toBe('extension-A');

      // --- Run with extension B ---
      shimFs._setPreopens({ '.': extDirB });
      shimCli._setEnv({});

      const modB = await import(join(outDirB, 'ext-shell.js'));
      expect(modB.hello()).toBe('extension-B');
    } finally {
      await rm(extBase, { recursive: true });
      await rm(outDirA, { recursive: true }).catch(() => {});
      await rm(outDirB, { recursive: true }).catch(() => {});
    }
  });
});
