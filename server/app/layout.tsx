export const metadata = {
  title: 'esp-git',
  description: 'feeds a small screen on a desk',
}

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  )
}
