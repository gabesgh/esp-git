//not much to look at. the device is the real frontend, this is just somewhere
//to confirm the deployment is alive and configured.

function Row({ k, ok }: { k: string; ok: boolean }) {
  return (
    <li style={{ fontFamily: 'ui-monospace, monospace', lineHeight: 1.8 }}>
      <span style={{ color: ok ? '#2ea043' : '#f85149' }}>{ok ? '●' : '○'}</span> {k}
    </li>
  )
}

export default function Page() {
  const checks: [string, boolean][] = [
    ['GITHUB_TOKEN', !!process.env.GITHUB_TOKEN],
    ['GITHUB_LOGIN', !!process.env.GITHUB_LOGIN],
    ['DEVICE_TOKEN', !!process.env.DEVICE_TOKEN],
    ['GITHUB_REPO (optional)', !!process.env.GITHUB_REPO],
    ['UPSTASH (durable pulse)', !!process.env.UPSTASH_REDIS_REST_URL],
  ]

  return (
    <main style={{ padding: 32, fontFamily: 'system-ui, sans-serif', maxWidth: 520 }}>
      <h1 style={{ fontSize: 18, marginBottom: 4 }}>esp-git</h1>
      <p style={{ color: '#888', fontSize: 14, marginTop: 0 }}>
        endpoints: <code>/api/stats</code>, <code>/api/pulse</code>; both need{' '}
        <code>x-device-token</code>.
      </p>

      <ul style={{ listStyle: 'none', padding: 0, fontSize: 14 }}>
        {checks.map(([k, ok]) => (
          <Row key={k} k={k} ok={ok} />
        ))}
      </ul>
    </main>
  )
}
