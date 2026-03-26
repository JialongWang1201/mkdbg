/* mkdbg_git.c — mkdbg git sub-commands
 *
 * When built with -DMKDBG_USE_LG2=1 (the CMake default when the libgit2
 * submodule is present), local read operations use libgit2 directly so
 * no external git CLI is required.
 *
 * Without that flag the original subprocess path is compiled in, which
 * is used by build_mkdbg_native.sh (shell-based builds without libgit2).
 *
 * Command         MKDBG_USE_LG2=1          fallback (subprocess)
 * ─────────────── ────────────────────────  ─────────────────────
 * git status      git_status_list_new()     git status
 * git rev         git_repository_head()     git rev-parse HEAD
 * git new-branch  git_branch_create()       git checkout -b <name>
 * git worktree    subprocess always         git worktree add <path>
 * git push        subprocess always         git push -u origin HEAD
 *
 * SPDX-License-Identifier: MIT
 */

#include "mkdbg.h"

#ifdef MKDBG_USE_LG2
#  include <git2.h>
#endif

/* ── shared helper: resolve repo root from mkdbg config ──────────────────── */

void git_resolve_repo_root(const GitOptions *opts,
                           char *config_path,
                           size_t config_path_size,
                           char *repo_root,
                           size_t repo_root_size)
{
  MkdbgConfig config;
  const char *repo_name;
  const RepoConfig *repo;

  if (find_config_upward(config_path, config_path_size) != 0) {
    die("missing %s; run `mkdbg init` first", CONFIG_NAME);
  }
  if (load_config_file(config_path, &config) != 0) {
    die("invalid config: %s", config_path);
  }
  resolve_repo_name(&config, opts->repo, opts->target, &repo_name);
  repo = find_repo_const(&config, repo_name);
  if (repo == NULL) {
    die("repo `%s` not found in %s", repo_name, config_path);
  }
  resolve_repo_root(config_path, repo, repo_root, repo_root_size);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * libgit2 implementation path (MKDBG_USE_LG2)
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifdef MKDBG_USE_LG2

/* ── lg2_die: print libgit2 error and exit ───────────────────────────────── */

static void lg2_die(const char *context)
{
  const git_error *e = git_error_last();
  if (e) {
    fprintf(stderr, "mkdbg: %s: %s\n", context, e->message);
  } else {
    fprintf(stderr, "mkdbg: %s: unknown libgit2 error\n", context);
  }
  exit(1);
}

/* ── cmd_git_status ──────────────────────────────────────────────────────── */
/*
 * Displays the working-tree status using libgit2:
 *
 *   On branch <name>  (or "HEAD detached at <sha>")
 *   <M|A|D|?> <path>  — one line per changed / untracked file
 *   nothing to commit, working tree clean  (if no changes)
 *
 * Status flags (matching git's short format):
 *   M  modified (index or workdir)
 *   A  added to index
 *   D  deleted
 *   R  renamed
 *   U  unmerged
 *   ?  untracked
 */
int cmd_git_status(const GitOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  git_repository  *repo = NULL;
  git_status_list *list = NULL;
  git_reference   *head = NULL;

  git_resolve_repo_root(opts, config_path, sizeof(config_path),
                        repo_root, sizeof(repo_root));

  if (opts->dry_run) {
    printf("[dry-run] git status in %s\n", repo_root);
    return 0;
  }

  git_libgit2_init();

  if (git_repository_open(&repo, repo_root) < 0)
    lg2_die("git_repository_open");

  /* Branch / HEAD line */
  if (git_repository_head_unborn(repo)) {
    printf("On branch (no commits yet)\n");
  } else if (git_repository_head_detached(repo)) {
    char sha[GIT_OID_SHA1_HEXSIZE + 1];
    if (git_repository_head(&head, repo) == 0) {
      const git_oid *o = git_reference_target(head);
      if (o) {
        git_oid_tostr(sha, sizeof(sha), o);
        printf("HEAD detached at %.8s\n", sha);
      }
      git_reference_free(head);
      head = NULL;
    } else {
      printf("HEAD detached\n");
    }
  } else {
    if (git_repository_head(&head, repo) < 0)
      lg2_die("git_repository_head");
    printf("On branch %s\n", git_reference_shorthand(head));
    git_reference_free(head);
    head = NULL;
  }

  /* File status */
  git_status_options sopts;
  memset(&sopts, 0, sizeof(sopts));
  sopts.version = GIT_STATUS_OPTIONS_VERSION;
  sopts.show  = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
  sopts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED
              | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;

  if (git_status_list_new(&list, repo, &sopts) < 0)
    lg2_die("git_status_list_new");

  size_t count = git_status_list_entrycount(list);
  if (count == 0) {
    printf("nothing to commit, working tree clean\n");
  } else {
    for (size_t i = 0; i < count; i++) {
      const git_status_entry *e = git_status_byindex(list, i);
      if (!e) continue;

      char flag = '?';
      const char *path = NULL;

      if (e->status & (GIT_STATUS_INDEX_NEW | GIT_STATUS_WT_NEW))
        flag = '?';
      if (e->status & GIT_STATUS_INDEX_NEW)
        flag = 'A';
      if (e->status & (GIT_STATUS_INDEX_MODIFIED | GIT_STATUS_WT_MODIFIED))
        flag = 'M';
      if (e->status & (GIT_STATUS_INDEX_DELETED | GIT_STATUS_WT_DELETED))
        flag = 'D';
      if (e->status & (GIT_STATUS_INDEX_RENAMED | GIT_STATUS_WT_RENAMED))
        flag = 'R';
      if (e->status & (GIT_STATUS_INDEX_TYPECHANGE | GIT_STATUS_WT_TYPECHANGE))
        flag = 'T';
      if (e->status & GIT_STATUS_CONFLICTED)
        flag = 'U';

      if (e->head_to_index && e->head_to_index->new_file.path)
        path = e->head_to_index->new_file.path;
      else if (e->index_to_workdir && e->index_to_workdir->new_file.path)
        path = e->index_to_workdir->new_file.path;

      if (path)
        printf("  %c  %s\n", flag, path);
    }
  }

  git_status_list_free(list);
  git_repository_free(repo);
  git_libgit2_shutdown();
  return 0;
}

/* ── cmd_git_rev ─────────────────────────────────────────────────────────── */
/*
 * Prints the full SHA-1 of HEAD (equivalent to `git rev-parse HEAD`).
 */
int cmd_git_rev(const GitOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  git_repository *repo = NULL;
  git_reference  *head = NULL;
  char sha[GIT_OID_SHA1_HEXSIZE + 1];

  git_resolve_repo_root(opts, config_path, sizeof(config_path),
                        repo_root, sizeof(repo_root));

  if (opts->dry_run) {
    printf("[dry-run] git rev-parse HEAD in %s\n", repo_root);
    return 0;
  }

  git_libgit2_init();

  if (git_repository_open(&repo, repo_root) < 0)
    lg2_die("git_repository_open");

  if (git_repository_head(&head, repo) < 0)
    lg2_die("git_repository_head");

  const git_oid *oid = git_reference_target(
      git_reference_is_branch(head) ? head : head);
  if (!oid) {
    /* Symbolic ref — peel to commit */
    git_reference *peeled = NULL;
    if (git_reference_resolve(&peeled, head) < 0)
      lg2_die("git_reference_resolve");
    oid = git_reference_target(peeled);
    git_reference_free(peeled);
  }

  git_oid_tostr(sha, sizeof(sha), oid);
  printf("%s\n", sha);

  git_reference_free(head);
  git_repository_free(repo);
  git_libgit2_shutdown();
  return 0;
}

/* ── cmd_git_new_branch ──────────────────────────────────────────────────── */
/*
 * Creates and checks out a new branch from HEAD
 * (equivalent to `git checkout -b <name>`).
 */
int cmd_git_new_branch(const GitOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  git_repository *repo   = NULL;
  git_reference  *head   = NULL;
  git_commit     *commit = NULL;
  git_reference  *branch = NULL;
  char            ref_name[PATH_MAX];

  if (opts->branch_name == NULL || opts->branch_name[0] == '\0') {
    die("git new-branch requires a branch name");
  }

  git_resolve_repo_root(opts, config_path, sizeof(config_path),
                        repo_root, sizeof(repo_root));

  if (opts->dry_run) {
    printf("[dry-run] git checkout -b %s in %s\n",
           opts->branch_name, repo_root);
    return 0;
  }

  git_libgit2_init();

  if (git_repository_open(&repo, repo_root) < 0)
    lg2_die("git_repository_open");

  /* Resolve HEAD → commit */
  if (git_repository_head(&head, repo) < 0)
    lg2_die("git_repository_head (new-branch)");

  git_object *obj = NULL;
  if (git_reference_peel(&obj, head, GIT_OBJECT_COMMIT) < 0)
    lg2_die("git_reference_peel");
  commit = (git_commit *)obj;

  /* Create the branch */
  if (git_branch_create(&branch, repo, opts->branch_name, commit, 0) < 0)
    lg2_die("git_branch_create");

  /* Set HEAD to the new branch */
  snprintf(ref_name, sizeof(ref_name),
           "refs/heads/%s", opts->branch_name);
  if (git_repository_set_head(repo, ref_name) < 0)
    lg2_die("git_repository_set_head");

  printf("Switched to a new branch '%s'\n", opts->branch_name);

  git_reference_free(branch);
  git_reference_free(head);
  git_commit_free(commit);
  git_repository_free(repo);
  git_libgit2_shutdown();
  return 0;
}

/* ── cmd_git_worktree — subprocess always (libgit2 worktree API incomplete) ─ */

int cmd_git_worktree(const GitOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char *argv[] = {(char *)"git", (char *)"worktree", (char *)"add",
                  (char *)opts->path, NULL};

  if (opts->path == NULL || opts->path[0] == '\0') {
    die("git worktree requires a path");
  }
  git_resolve_repo_root(opts, config_path, sizeof(config_path),
                        repo_root, sizeof(repo_root));
  return run_process(argv, repo_root, opts->dry_run);
}

/* ── cmd_git_push_current — subprocess always (network op) ──────────────── */

int cmd_git_push_current(const GitOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char *argv[] = {(char *)"git", (char *)"push", (char *)"-u",
                  (char *)"origin", (char *)"HEAD", NULL};

  git_resolve_repo_root(opts, config_path, sizeof(config_path),
                        repo_root, sizeof(repo_root));
  return run_process(argv, repo_root, opts->dry_run);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * subprocess fallback (no MKDBG_USE_LG2) — original behaviour
 * ═══════════════════════════════════════════════════════════════════════════ */
#else /* !MKDBG_USE_LG2 */

int cmd_git_status(const GitOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char *argv[] = {(char *)"git", (char *)"status", NULL};

  git_resolve_repo_root(opts, config_path, sizeof(config_path),
                        repo_root, sizeof(repo_root));
  return run_process(argv, repo_root, opts->dry_run);
}

int cmd_git_rev(const GitOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char *argv[] = {(char *)"git", (char *)"rev-parse", (char *)"HEAD", NULL};

  git_resolve_repo_root(opts, config_path, sizeof(config_path),
                        repo_root, sizeof(repo_root));
  return run_process(argv, repo_root, opts->dry_run);
}

int cmd_git_new_branch(const GitOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char *argv[] = {(char *)"git", (char *)"checkout", (char *)"-b",
                  (char *)opts->branch_name, NULL};

  if (opts->branch_name == NULL || opts->branch_name[0] == '\0') {
    die("git new-branch requires a branch name");
  }
  git_resolve_repo_root(opts, config_path, sizeof(config_path),
                        repo_root, sizeof(repo_root));
  return run_process(argv, repo_root, opts->dry_run);
}

int cmd_git_worktree(const GitOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char *argv[] = {(char *)"git", (char *)"worktree", (char *)"add",
                  (char *)opts->path, NULL};

  if (opts->path == NULL || opts->path[0] == '\0') {
    die("git worktree requires a path");
  }
  git_resolve_repo_root(opts, config_path, sizeof(config_path),
                        repo_root, sizeof(repo_root));
  return run_process(argv, repo_root, opts->dry_run);
}

int cmd_git_push_current(const GitOptions *opts)
{
  char config_path[PATH_MAX];
  char repo_root[PATH_MAX];
  char *argv[] = {(char *)"git", (char *)"push", (char *)"-u",
                  (char *)"origin", (char *)"HEAD", NULL};

  git_resolve_repo_root(opts, config_path, sizeof(config_path),
                        repo_root, sizeof(repo_root));
  return run_process(argv, repo_root, opts->dry_run);
}

#endif /* MKDBG_USE_LG2 */
