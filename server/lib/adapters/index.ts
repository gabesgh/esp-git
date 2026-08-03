//everything the device can render, and nothing about where it came from.
//add a new adapter by implementing Adapter and dropping it in the registry below.

export type Level = 0 | 1 | 2 | 3 | 4

export type Heatmap = {
  //one level per day, oldest first. 371 entries for a full GitHub year.
  levels: Level[]
  total: number
  from: string
  to: string
}

export type Series = {
  labels: string[]
  values: number[]
}

export type Stat = {
  label: string
  value: string | number
  //optional accent, device maps this to a colour. keep it to a handful.
  tone?: 'neutral' | 'good' | 'warn' | 'bad'
}

export type Snapshot = {
  heatmap?: Heatmap
  series?: Series
  stats?: Stat[]
  //rendered as a list under the grid. computed server side so the device never
  //has to hold the raw daily counts. icon is a name, not a character - the
  //panel's fonts are ascii only so the firmware draws these as shapes.
  notes?: Note[]
}

export type NoteIcon = 'today' | 'up' | 'down' | 'flame' | 'star' | 'trophy'

export type Note = {
  icon: NoteIcon
  text: string
}

export interface Adapter {
  id: string
  label: string
  load(): Promise<Snapshot>
}

/**
 * Registry. The public build ships github only.
 *
 * Private adapters are resolved at runtime from ./private/, which is gitignored,
 * so a fork never carries anyone else's data source and this file never needs
 * editing to keep something out of the repo.
 */
export async function resolveAdapters(ids: string[]): Promise<Adapter[]> {
  const out: Adapter[] = []

  for (const id of ids) {
    if (id === 'github') {
      const { githubAdapter } = await import('./github')
      out.push(githubAdapter())
      continue
    }

    try {
      const mod = await import(`./private/${id}`)
      out.push(mod.default())
    } catch {
      //not fatal. a missing private adapter just means that panel is skipped.
      console.warn(`adapter "${id}" not found, skipping`)
    }
  }

  return out
}

//the wire format. deliberately tiny; this goes to a device with ~200KB of usable heap.
//levels ship as a digit string ("0013420...") rather than an array of ints, which
//saves roughly 3x over JSON numbers and stays readable in curl.
export function pack(snap: Snapshot) {
  const out: Record<string, unknown> = {}

  if (snap.heatmap) {
    out.cal = snap.heatmap.levels.join('')
    out.total = snap.heatmap.total
    out.from = snap.heatmap.from
    out.to = snap.heatmap.to
  }
  if (snap.series) {
    out.labels = snap.series.labels
    out.values = snap.series.values
  }
  if (snap.stats?.length) {
    out.stats = snap.stats.map((s) => [s.label, s.value, s.tone ?? 'neutral'])
  }
  if (snap.notes?.length) {
    out.notes = snap.notes.map((n) => [n.icon, n.text])
  }

  return out
}
