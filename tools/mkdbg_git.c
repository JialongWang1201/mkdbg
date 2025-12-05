#include "mkdbg.h"

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
