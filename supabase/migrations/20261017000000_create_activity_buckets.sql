-- activity_buckets: one row per device per minute.
--
-- This records the schema live in the Supabase project (internetbutton,
-- jdewkdjgbghtvzwsmpuw), dumped with `supabase db dump`, so the repo is
-- self-contained. The design and rationale are in docs/ACTIVITY.md
-- ("The cloud end").

create table public.activity_buckets (
  id           bigint not null,
  device_id    text not null,
  bucket_start timestamptz not null,
  impulses     integer not null,
  impulse_sum  bigint default 0 not null,
  max_delta    integer,
  max_dz       integer,
  batch        integer,
  received_at  timestamptz default now() not null,

  constraint activity_buckets_impulse_sum_check check (impulse_sum >= 0),
  constraint activity_buckets_impulses_check     check (impulses >= 0),
  constraint activity_buckets_max_delta_check    check (max_delta >= 0),
  constraint activity_buckets_max_dz_check       check (max_dz >= 0)
);

comment on table public.activity_buckets is
  'One row per minute of accelerometer activity, as published by firmware/activity-log.';

comment on column public.activity_buckets.max_dz is
  'Largest |dz| in the bucket. Saturates at 255 in firmware v1 (uint8), so 255 means "at least".';

alter table public.activity_buckets
  alter column id add generated always as identity;

alter table only public.activity_buckets
  add constraint activity_buckets_pkey primary key (id);

alter table only public.activity_buckets
  add constraint activity_buckets_unique unique (device_id, bucket_start);

create index activity_buckets_start_idx
  on public.activity_buckets using btree (bucket_start desc);

-- RLS is enabled with no policies: anon and authenticated roles can read and
-- write nothing. Only the service role (used inside the ingest-activity Edge
-- Function) inserts. This is deliberate, not an oversight.
alter table public.activity_buckets enable row level security;
