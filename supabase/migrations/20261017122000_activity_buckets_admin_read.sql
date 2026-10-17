-- Read-only access for the admin dashboard.
--
-- This migration only adds metadata to the existing `activity_buckets` table:
--   1. A read-only SELECT policy for the `authenticated` role.
--   2. Membership in the `supabase_realtime` publication for live updates.
--
-- It does NOT modify the table's schema or data.

-- 1. Allow logged-in (authenticated) users to read activity buckets.
drop policy if exists "activity_buckets_select_authenticated" on public.activity_buckets;

create policy "activity_buckets_select_authenticated"
  on public.activity_buckets
  for select
  to authenticated
  using (true);

-- 2. Add the table to the realtime publication so postgres_changes emits events.
do $$
begin
  if not exists (
    select 1
    from pg_publication_tables
    where pubname = 'supabase_realtime'
      and schemaname = 'public'
      and tablename = 'activity_buckets'
  ) then
    alter publication supabase_realtime add table public.activity_buckets;
  end if;
end
$$;
