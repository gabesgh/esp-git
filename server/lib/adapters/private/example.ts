import type { Adapter, Snapshot } from '../index'

/**
 * Template for a private adapter, and the one tracked file in this directory.
 *
 * Everything else here is gitignored, so a fork never carries someone else's
 * data sources. This file stays because the registry reaches this directory
 * through a dynamic import, and a bundler cannot resolve that against a folder
 * with nothing in it; without one real module the build warns on every run.
 *
 * To write your own: copy this to `mine.ts`, return whatever you can fetch, and
 * add `mine` to ADAPTERS. Nothing else needs editing and the firmware does not
 * change. Return a heatmap, a series, some stats, notes, or any combination.
 */
export default function exampleAdapter(): Adapter {
  return {
    id: 'example',
    label: 'example',

    async load(): Promise<Snapshot> {
      return {
        stats: [
          { label: 'answer', value: 42, tone: 'good' },
          { label: 'source', value: 'static' },
        ],
        notes: [{ icon: 'star', text: 'replace this with something real' }],
      }
    },
  }
}
