import { strictEqual } from 'node:assert';

export const noSnapshot = true;

export const disableFeatures = ['random', 'clocks', 'http', 'stdio'];

export function test(instance) {
  strictEqual(instance.hello(), 'Hello from no-snapshot mode!');
}
