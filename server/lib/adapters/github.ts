import type { Adapter, Level, Note, Snapshot } from './index'

const API = 'https://api.github.com/graphql'

//GitHub hands back a hex colour per day rather than a level, so map it back.
//these are the light-theme ramp values; they've been stable for years but the
//fallback below means a palette change degrades instead of breaking.
const RAMP: Record<string, Level> = {
  '#ebedf0': 0,
  '#9be9a8': 1,
  '#40c463': 2,
  '#30a14e': 3,
  '#216e39': 4,
}

function levelFor(color: string, count: number): Level {
  const hit = RAMP[color.toLowerCase()]
  if (hit !== undefined) return hit

  if (count === 0) return 0
  if (count < 3) return 1
  if (count < 7) return 2
  if (count < 12) return 3

  return 4
}

/**
 * GitClear, "Developer Report": 878,592 dev-years analysed. The median developer
 * who commits at least once per working day lands 417 commits across 156 active
 * days, so ~2.67 per active day.
 *
 * Only used when SHOW_BENCHMARK is set, and the wording says "contributions vs
 * commits" out loud, because they are not the same unit. GitHub's calendar counts
 * commits + issues + PRs + reviews, and totalCommitContributions omits private
 * repos entirely, so there is no honest way to get a like-for-like commit count
 * out of the API. Treat it as a rough scale marker, not a measurement.
 */
const GITCLEAR_MEDIAN_YEAR = 417

//how far back "usual" looks. a quarter is long enough to survive a quiet week
//without being so long that last winter still drags the number around.
const BASELINE_DAYS = 90

function buildNotes(days: { date: string; contributionCount: number }[], yearTotal: number): Note[] {
  const notes: Note[] = []
  if (!days.length) return notes

  const counts = days.map((d) => d.contributionCount)
  const today = counts[counts.length - 1]

  const window = counts.slice(-BASELINE_DAYS)
  const avg = window.reduce((a, b) => a + b, 0) / window.length

  // streak walks back from today. today itself being empty means zero, not
  // "yesterday's streak", which is the answer people actually expect at 9am.
  let streak = 0
  for (let i = counts.length - 1; i >= 0 && counts[i] > 0; i--) streak++

  notes.push({ icon: 'today', text: `${today} today` })

  if (avg >= 0.5) {
    const ratio = today / avg
    if (today === 0) notes.push({ icon: 'down', text: `avg ${avg.toFixed(1)} a day` })
    else if (ratio >= 1.15) notes.push({ icon: 'up', text: `${ratio.toFixed(1)}x your ${BASELINE_DAYS}d avg` })
    else if (ratio <= 0.85) notes.push({ icon: 'down', text: `${Math.round(ratio * 100)}% of ${BASELINE_DAYS}d avg` })
    else notes.push({ icon: 'up', text: `on par with your ${BASELINE_DAYS}d avg` })
  }

  if (streak > 1) notes.push({ icon: 'flame', text: `${streak} day streak` })

  const best = Math.max(...counts)
  const rank = counts.filter((c) => c > today).length
  const pct = Math.max(Math.round((rank / counts.length) * 100), 1)

  if (today > 0 && pct <= 25) notes.push({ icon: 'trophy', text: `top ${pct}% of your days` })
  else notes.push({ icon: 'star', text: `best day ${best}` })

  if (process.env.SHOW_BENCHMARK) {
    const x = yearTotal / GITCLEAR_MEDIAN_YEAR
    notes.push({ icon: 'up', text: `${x.toFixed(1)}x median dev commits/yr` })
  }

  //four is what fits under the grid without crowding it
  return notes.slice(0, 4)
}

const CALENDAR = `
query($login: String!) {
  user(login: $login) {
    contributionsCollection {
      contributionYears
      contributionCalendar {
        totalContributions
        weeks { contributionDays { date contributionCount color } }
      }
    }
  }
}`

const YEAR = `
query($login: String!, $from: DateTime!, $to: DateTime!) {
  user(login: $login) {
    contributionsCollection(from: $from, to: $to) {
      contributionCalendar { totalContributions }
    }
  }
}`

const REPO = `
query($owner: String!, $name: String!) {
  repository(owner: $owner, name: $name) {
    issues(states: OPEN) { totalCount }
    pullRequests(states: OPEN) { totalCount }
    stargazerCount
  }
}`

async function gql<T>(query: string, variables: Record<string, unknown>): Promise<T> {
  const token = process.env.GITHUB_TOKEN
  if (!token) throw new Error('GITHUB_TOKEN is not set')

  const res = await fetch(API, {
    method: 'POST',
    headers: {
      authorization: `bearer ${token}`,
      'content-type': 'application/json',
      //GitHub rejects requests without one
      'user-agent': 'esp-git',
    },
    body: JSON.stringify({ query, variables }),
    //route-level caching handles freshness; don't let fetch double-cache
    cache: 'no-store',
  })

  if (!res.ok) throw new Error(`github ${res.status}: ${await res.text()}`)

  const body = await res.json()
  if (body.errors) throw new Error(`github graphql: ${JSON.stringify(body.errors)}`)

  return body.data as T
}

export function githubAdapter(): Adapter {
  const login = process.env.GITHUB_LOGIN
  const repo = process.env.GITHUB_REPO //"owner/name", optional

  return {
    id: 'github',
    label: 'GitHub',

    async load(): Promise<Snapshot> {
      if (!login) throw new Error('GITHUB_LOGIN is not set')

      const cal = await gql<any>(CALENDAR, { login })
      const cc = cal.user.contributionsCollection
      const days = cc.contributionCalendar.weeks.flatMap((w: any) => w.contributionDays)

      const heatmap = {
        levels: days.map((d: any) => levelFor(d.color, d.contributionCount)) as Level[],
        total: cc.contributionCalendar.totalContributions,
        from: days[0].date,
        to: days[days.length - 1].date,
      }

      //per-year totals. cap it so a decade-old account doesn't fan out forever.
      const years: number[] = cc.contributionYears.slice(0, 6).reverse()
      const totals = await Promise.all(
        years.map(async (y) => {
          const r = await gql<any>(YEAR, {
            login,
            from: `${y}-01-01T00:00:00Z`,
            to: `${y}-12-31T23:59:59Z`,
          })
          return r.user.contributionsCollection.contributionCalendar.totalContributions
        }),
      )

      const stats = []

      //today is the last cell of the calendar
      const today = days[days.length - 1]
      stats.push({
        label: 'today',
        value: today.contributionCount,
        tone: today.contributionCount > 0 ? ('good' as const) : ('neutral' as const),
      })

      // The repo panel is a nice-to-have. A renamed account or a deleted repo
      // should cost you that one panel, not the contribution graph as well,
      // which is what happens if this is allowed to throw.
      if (repo) {
        try {
          const [owner, name] = repo.split('/')
          const r = await gql<any>(REPO, { owner, name })
          stats.push({ label: 'open issues', value: r.repository.issues.totalCount, tone: 'warn' as const })
          stats.push({ label: 'open PRs', value: r.repository.pullRequests.totalCount })
          stats.push({ label: 'stars', value: r.repository.stargazerCount })
        } catch (err) {
          console.warn(`repo panel for ${repo} failed, skipping:`, err)
          stats.push({ label: 'repo', value: 'not found', tone: 'warn' as const })
        }
      }

      return {
        heatmap,
        series: { labels: years.map(String), values: totals },
        stats,
        notes: buildNotes(days, heatmap.total),
      }
    },
  }
}
