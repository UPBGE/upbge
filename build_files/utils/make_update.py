#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2019-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
"make update" for all platforms, updating Git LFS submodules for libraries and
Blender git repository.

For release branches, this will check out the appropriate branches of
submodules and libraries.

WARNING:
- Python 3.6 is used on the Linux VM (Rocky8) to run "make update" to checkout LFS.
- Python 3.9 is used on the built-bot.

Take care *not* to use features from the Python version used by Blender!

NOTE:
Some type annotations are quoted to avoid errors in older Python versions.
These can be unquoted eventually.
"""

__all__ = (
    "main",
)


import argparse
import os
import platform
import shutil
import sys

import make_utils
from pathlib import Path
from make_utils import call, check_output
from urllib.parse import urljoin, urlsplit


def print_stage(text: str) -> None:
    print("")
    print(text)
    print("=" * len(text))
    print("")


def parse_arguments() -> argparse.Namespace:
    """
    Parse command line arguments.

    Returns parsed object from which the command line arguments can be accessed
    as properties. The name of the properties matches the command line argument,
    but with the leading dashed omitted and all remaining dashes replaced with
    underscore.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument("--no-libraries", action="store_true",
                        help="Don't fetch precompiled libraries for this system")
    parser.add_argument("--no-blender", action="store_true", help="Don't update the Blender code repository")
    parser.add_argument(
        "--no-lfs-fallback",
        action="store_true",
        help="Don't set up fallback URLs for fetching LFS files from projects.blender.org. These are only used when cloning repositories hosted elsewhere.")
    parser.add_argument(
        "--git-command",
        default="git",
        help="Path to the git binary. (Only useful if it is not in your PATH)")
    parser.add_argument("--architecture", type=str,
                        choices=("x86_64", "amd64", "arm64",))
    parser.add_argument("--prune-destructive", action="store_true",
                        help="Destructive! Detect and remove stale files from older checkouts")

    # Deprecated options, kept for compatibility with old configurations.
    parser.add_argument("--use-tests", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--no-submodules", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--use-linux-libraries", action="store_true", help=argparse.SUPPRESS)

    return parser.parse_args()


def get_blender_git_root(args: argparse.Namespace) -> Path:
    """
    Get root directory of the current Git directory.
    """
    return Path(
        check_output([args.git_command, "rev-parse", "--show-toplevel"]))


def get_effective_platform(args: argparse.Namespace) -> str:
    """
    Get platform of the host.

    The result string is normalized to the name used by Blender releases and
    library repository name prefixes: linux, macos, windows.
    """

    if sys.platform == "darwin":
        platform = "macos"
    elif sys.platform == "win32":
        platform = "windows"
    else:
        platform = sys.platform

    assert (platform in ("linux", "macos", "windows"))

    return platform


def get_effective_architecture(args: argparse.Namespace) -> str:
    """
    Get architecture of the host.

    The result string is normalized to the architecture name used by the Blender
    releases and library repository name suffixes: x64, arm64.

    NOTE: When cross-compiling the architecture is coming from the command line
    argument.
    """
    architecture: "str | None" = args.architecture
    if architecture:
        assert isinstance(architecture, str)
    elif "ARM64" in platform.version():
        # Check platform.version to detect arm64 with x86_64 python binary.
        architecture = "arm64"
    else:
        architecture = platform.machine().lower()

    # Normalize the architecture name.
    if architecture in {"x86_64", "amd64"}:
        architecture = "x64"
    if architecture == "aarch64":
        architecture = "arm64"

    assert isinstance(architecture, str)
    return architecture


# NOTE: unquote "tuple" once Python 3.6x is dropped.
def get_submodule_directories(args: argparse.Namespace) -> "tuple[Path, ...]":
    """
    Get list of all configured submodule directories.
    """

    blender_git_root = get_blender_git_root(args)
    dot_modules = blender_git_root / ".gitmodules"

    if not dot_modules.exists():
        return ()

    submodule_directories_output = check_output(
        [args.git_command, "config", "--file", str(dot_modules), "--get-regexp", "path"])
    return tuple([Path(line.split(' ', 1)[1]) for line in submodule_directories_output.strip().splitlines()])


def ensure_git_lfs(args: argparse.Namespace) -> None:
    # Use `--skip-repo` to avoid creating git hooks.
    # This is called from the `blender.git` checkout, so we don't need to install hooks there.
    call((args.git_command, "lfs", "install", "--skip-repo"), exit_on_error=True)


def switch_blender_git_remotes(args: argparse.Namespace) -> None:
    """
    Switch remote URLs from projects.blender.org to git.blender.org
    """
    remotes = make_utils.git_get_remotes(args.git_command)

    for remote in remotes:
        url = make_utils.git_get_remote_url(args.git_command, remote)
        new_url = url.replace("git@projects.blender.org", "git@git.blender.org")

        if new_url == url:
            continue

        print(f"Replacing {remote} URL from {url} to {new_url}")
        make_utils.git_set_config(args.git_command, f"remote.{remote}.url", new_url)


def prune_stale_files(args: argparse.Namespace) -> None:
    """
    Ensure files from previous Git configurations do not exist anymore
    """
    print_stage("Removing stale files")

    blender_git_root = get_blender_git_root(args)
    found_stale_files = False

    for relative_dir_to_remove in (
        Path("scripts") / "addons",
        Path("scripts") / "addons_contrib",
    ):
        dir_to_remove = blender_git_root / relative_dir_to_remove
        if not dir_to_remove.exists():
            continue
        if not dir_to_remove.is_dir():
            print(f"'{relative_dir_to_remove}' exists but is not a directory")
            continue
        print(f"Removing '{relative_dir_to_remove}'")
        make_utils.remove_directory(dir_to_remove)
        found_stale_files = True

    if not found_stale_files:
        print("Checkout looks pristine")


def initialize_precompiled_libraries(args: argparse.Namespace) -> str:
    """
    Configure submodule for precompiled libraries

    This function detects the current host architecture and enables
    corresponding submodule, and updates the submodule.

    NOTE: When cross-compiling the architecture is coming from the command line
    argument.
    """

    print_stage("Configuring Precompiled Libraries")

    platform = get_effective_platform(args)
    arch = get_effective_architecture(args)

    print(f"Detected platform     : {platform}")
    print(f"Detected architecture : {arch}")
    print()

    submodule_dir = f"lib/{platform}_{arch}"

    submodule_directories = get_submodule_directories(args)

    if platform == "macos" and arch == "x64":
        return ("WARNING: macOS x64/Intel support was dropped in Blender 5.0.\n"
                "         As such, pre-compiled dependencies are no longer provided.\n"
                "         You may build the dependencies yourself, or downgrade to Blender 4.5.\n"
                "         For more details, please see: https://devtalk.blender.org/t/38835")

    if Path(submodule_dir) not in submodule_directories:
        return "Skipping libraries update: no configured submodule\n"

    print(f"* Enabling precompiled libraries at {submodule_dir}")
    make_utils.git_enable_submodule(args.git_command, Path(submodule_dir))

    return ""


def git_update_skip(args: argparse.Namespace, check_remote_exists: bool = True) -> str:
    """Test if git repo can be updated."""

    if make_utils.command_missing(args.git_command):
        sys.stderr.write("git not found, can't update code\n")
        sys.exit(1)

    # Abort if a rebase is still progress.
    rebase_merge = check_output([args.git_command, 'rev-parse', '--git-path', 'rebase-merge'], exit_on_error=False)
    rebase_apply = check_output([args.git_command, 'rev-parse', '--git-path', 'rebase-apply'], exit_on_error=False)
    merge_head = check_output([args.git_command, 'rev-parse', '--git-path', 'MERGE_HEAD'], exit_on_error=False)
    if (
            os.path.exists(rebase_merge) or
            os.path.exists(rebase_apply) or
            os.path.exists(merge_head)
    ):
        return "rebase or merge in progress, complete it first"

    # Abort if uncommitted changes.
    changes = check_output([args.git_command, 'status', '--porcelain', '--untracked-files=no', '--ignore-submodules'])
    if len(changes) != 0:
        return "you have unstaged changes"

    # Test if there is an upstream branch configured
    if check_remote_exists:
        branch = check_output([args.git_command, "rev-parse", "--abbrev-ref", "HEAD"])
        remote = check_output([args.git_command, "config", "branch." + branch + ".remote"], exit_on_error=False)
        if len(remote) == 0:
            return "no remote branch to pull from"

    return ""


def use_upstream_workflow(args: argparse.Namespace) -> bool:
    return make_utils.git_remote_exist(args.git_command, "upstream")


def work_tree_update_upstream_workflow(args: argparse.Namespace, use_fetch: bool = True) -> str:
    """
    Update the Blender repository using the GitHub style of fork organization

    Returns true if the current local branch has been updated to the upstream state.
    Otherwise false is returned.
    """

    branch_name = make_utils.git_branch(args.git_command)

    if use_fetch:
        call((args.git_command, "fetch", "upstream"))

    upstream_branch = f"upstream/{branch_name}"
    if not make_utils.git_branch_exists(args.git_command, upstream_branch):
        return "no_branch"

    retcode = call((args.git_command, "merge", "--ff-only", upstream_branch), exit_on_error=False)
    if retcode != 0:
        return "Unable to fast forward\n"

    return ""


def work_tree_update(args: argparse.Namespace, use_fetch: bool = True) -> str:
    """
    Update the Git working tree using the best strategy

    This function detects whether it is a github style of fork remote organization is used, or
    is it a repository which origin is an upstream.
    """

    if use_upstream_workflow(args):
        message = work_tree_update_upstream_workflow(args, use_fetch)
        if message != "no_branch":
            return message

        # If there is upstream configured but the local branch is not in the upstream, try to
        # update the branch from the fork.

    update_command = [args.git_command, "pull", "--rebase"]

    call(update_command)

    return ""


# Update blender repository.
def blender_update(args: argparse.Namespace) -> str:
    print_stage("Updating Blender Git Repository")

    return work_tree_update(args)


# Extra LFS update for blender repository
def blender_lfs_update(args: argparse.Namespace) -> None:
    print_stage("Updating Blender Git LFS")

    # This seems to be required some times, e.g. on initial checkout from third party, non-lfs repository
    # (like the github one). The fallback repository set by `lfs_fallback_setup` is fetched, but running the
    # `update_command` above does not seem to do the actual checkout for these LFS-managed files.
    update_lfs_command = [args.git_command, "lfs", "pull"]
    call(update_lfs_command)


def resolve_external_url(blender_url: str, repo_name: str) -> str:
    return urljoin(blender_url + "/", "../" + repo_name)


def external_script_copy_old_submodule_over(
        args: argparse.Namespace,
        directory: Path,
        old_submodules_dir: Path,
) -> None:
    blender_git_root = get_blender_git_root(args)
    external_dir = blender_git_root / directory

    print(f"Moving {old_submodules_dir} to {directory} ...")
    shutil.move(blender_git_root / old_submodules_dir, external_dir)

    # Remove old ".git" which is a file with path to a submodule bare repo inside of main
    # repo .git/modules directory.
    (external_dir / ".git").unlink()

    bare_repo_relative_dir = Path(".git") / "modules" / old_submodules_dir
    print(f"Copying {bare_repo_relative_dir} to {directory}/.git ...")
    bare_repo_dir = blender_git_root / bare_repo_relative_dir
    shutil.copytree(bare_repo_dir, external_dir / ".git")

    git_config = external_dir / ".git" / "config"
    call((args.git_command, "config", "--file", str(git_config), "--unset", "core.worktree"))


def floating_checkout_initialize_if_needed(
        args: argparse.Namespace,
        repo_name: str,
        directory: Path,
        old_submodules_dir: "Path | None" = None,
) -> None:
    """Initialize checkout of an external repository"""

    blender_git_root = get_blender_git_root(args)
    blender_dot_git = blender_git_root / ".git"
    external_dir = blender_git_root / directory

    if external_dir.exists():
        return

    print(f"Initializing {directory} ...")

    if old_submodules_dir is not None:
        old_submodule_dot_git = blender_git_root / old_submodules_dir / ".git"
        if old_submodule_dot_git.exists() and blender_dot_git.is_dir():
            external_script_copy_old_submodule_over(args, directory, old_submodules_dir)
            return

    origin_name = "upstream" if use_upstream_workflow(args) else "origin"
    blender_url = make_utils.git_get_remote_url(args.git_command, origin_name)
    external_url = resolve_external_url(blender_url, repo_name)

    # When running `make update` from a freshly cloned fork check whether the fork of the submodule is
    # available, If not, switch to the submodule relative to the main blender repository.
    if origin_name == "origin" and not make_utils.git_is_remote_repository(args.git_command, external_url):
        external_url = resolve_external_url("https://projects.blender.org/blender/blender", repo_name)

    call((args.git_command, "clone", "--origin", origin_name, external_url, str(external_dir)))


def floating_checkout_add_origin_if_needed(
        args: argparse.Namespace,
        repo_name: str,
        directory: Path,
) -> None:
    """
    Add remote called 'origin' if there is a fork of the external repository available

    This is only done when using Github style upstream workflow in the main repository.
    """

    if not use_upstream_workflow(args):
        return

    cwd = os.getcwd()

    blender_git_root = get_blender_git_root(args)
    external_dir = blender_git_root / directory

    origin_blender_url = make_utils.git_get_remote_url(args.git_command, "origin")
    origin_external_url = resolve_external_url(origin_blender_url, repo_name)

    try:
        os.chdir(external_dir)

        if (make_utils.git_remote_exist(args.git_command, "origin") or
                not make_utils.git_remote_exist(args.git_command, "upstream")):
            return

        if not make_utils.git_is_remote_repository(args.git_command, origin_external_url):
            return

        print(f"Adding origin remote to {directory} pointing to fork ...")

        # Non-obvious tricks to introduce the new remote called "origin" to the existing
        # submodule configuration.
        #
        # This is all within the content of creating a fork of a submodule after `make update`
        # has been run and possibly local branches tracking upstream were added.
        #
        # The idea here goes as following:
        #
        #  - Rename remote "upstream" to "origin", which takes care of changing the names of
        #    remotes the local branches are tracking.
        #
        #  - Change the URL to the "origin", which was still pointing to upstream.
        #
        #  - Re-introduce the "upstream" remote, with the same URL as it had prior to rename.

        upstream_url = make_utils.git_get_remote_url(args.git_command, "upstream")

        call((args.git_command, "remote", "rename", "upstream", "origin"))
        make_utils.git_set_config(args.git_command, "remote.origin.url", origin_external_url)

        call((args.git_command, "remote", "add", "upstream", upstream_url))
    finally:
        os.chdir(cwd)

    return


def floating_checkout_update(
        args: argparse.Namespace,
        repo_name: str,
        directory: Path,
        branch: "str | None",
        old_submodules_dir: "Path | None" = None,
        only_update: bool = False,
) -> str:
    """Update a single external checkout with the given name in the scripts folder"""

    blender_git_root = get_blender_git_root(args)
    external_dir = blender_git_root / directory

    if only_update and not external_dir.exists():
        return ""

    floating_checkout_initialize_if_needed(args, repo_name, directory, old_submodules_dir)
    floating_checkout_add_origin_if_needed(args, repo_name, directory)

    blender_git_root = get_blender_git_root(args)
    external_dir = blender_git_root / directory

    print(f"* Updating {directory} ...")

    cwd = os.getcwd()

    # Update externals to appropriate given branch, falling back to main if none is given and/or
    # found in a sub-repository.
    branch_fallback = "main"
    if not branch:
        branch = branch_fallback

    skip_msg = ""

    try:
        os.chdir(external_dir)
        msg = git_update_skip(args, check_remote_exists=False)
        if msg:
            skip_msg += str(directory) + " skipped: " + msg + "\n"
        else:
            # Find a matching branch that exists.
            for remote in ("origin", "upstream"):
                if make_utils.git_remote_exist(args.git_command, remote):
                    call([args.git_command, "fetch", remote])

            submodule_branch = branch

            if make_utils.git_branch_exists(args.git_command, submodule_branch):
                pass
            elif make_utils.git_branch_exists(args.git_command, branch_fallback):
                submodule_branch = branch_fallback
            else:
                # Skip.
                submodule_branch = ""

            # Switch to branch and pull.
            if submodule_branch:
                if make_utils.git_branch(args.git_command) != submodule_branch:
                    # If the local branch exists just check out to it.
                    # If there is no local branch but only remote specify an explicit remote.
                    # Without this explicit specification Git attempts to set-up tracking
                    # automatically and fails when the branch is available in multiple remotes.
                    if make_utils.git_local_branch_exists(args.git_command, submodule_branch):
                        call([args.git_command, "checkout", submodule_branch])
                    else:
                        if make_utils.git_remote_branch_exists(args.git_command, "origin", submodule_branch):
                            call([args.git_command, "checkout", "-t", f"origin/{submodule_branch}"])
                        elif make_utils.git_remote_exist(args.git_command, "upstream"):
                            # For the Github style of upstream workflow create a local branch from
                            # the upstream, but do not track it, so that we stick to the paradigm
                            # that no local branches are tracking upstream, preventing possible
                            # accidental commit to upstream.
                            call([args.git_command, "checkout", "-b", submodule_branch,
                                 f"upstream/{submodule_branch}", "--no-track"])

                # Don't use extra fetch since all remotes of interest have been already fetched
                # some lines above.
                skip_msg += work_tree_update(args, use_fetch=False)
    finally:
        os.chdir(cwd)

    return skip_msg


def add_submodule_push_url(args: argparse.Namespace) -> None:
    """
    Add pushURL configuration for all locally activated submodules, pointing to SSH protocol.
    """

    blender_git_root = get_blender_git_root(args)
    modules = blender_git_root / ".git" / "modules"

    submodule_directories = get_submodule_directories(args)

    for submodule_path in submodule_directories:
        module_path = modules / submodule_path
        config = module_path / "config"

        if not config.exists():
            # Ignore modules which are not initialized
            continue

        push_url = check_output((args.git_command, "config", "--file", str(config),
                                "--get", "remote.origin.pushURL"), exit_on_error=False)

        # Don't modify PushURL if it is set.
        if push_url:
            if "projects.blender.org" in push_url:
                # Allow the code below to replace projects.blender.org with git.blender.org
                pass
            else:
                continue

        url = make_utils.git_get_config(args.git_command, "remote.origin.url", str(config))
        if not url.startswith("https:"):
            # Ignore non-URL URLs.
            continue

        url_parts = urlsplit(url)

        host = url_parts.hostname
        if host == "projects.blender.org":
            host = "git.blender.org"

        push_url = f"git@{host}:{url_parts.path[1:]}"

        print(f"Setting pushURL to {push_url} for {submodule_path}")
        make_utils.git_set_config(args.git_command, "remote.origin.pushURL", push_url, str(config))


def submodules_lib_update(args: argparse.Namespace, branch: "str | None") -> str:
    print_stage("Updating Libraries")

    msg = ""

    submodule_directories = get_submodule_directories(args)
    for submodule_path in submodule_directories:
        if not make_utils.is_git_submodule_enabled(args.git_command, submodule_path):
            print(f"* Skipping {submodule_path}")
            continue

        print(f"* Updating {submodule_path} ...")

        if not make_utils.git_update_submodule(args.git_command, submodule_path):
            msg += f"Error updating Git submodule {submodule_path}\n"

    add_submodule_push_url(args)

    return msg


def lfs_fallback_setup(args: argparse.Namespace) -> None:
    """
    Set up an additional projects.blender.org remote, for LFS fetching fallback
    in case the fork does not include LFS files.
    """
    remotes = make_utils.git_get_remotes(args.git_command)
    add_fallback_remote = True
    fallback_remote = "lfs-fallback"

    for remote in remotes:
        url = make_utils.git_get_remote_url(args.git_command, remote)
        if "projects.blender.org" not in url and "git.blender.org" not in url:
            make_utils.git_set_config(args.git_command, f"lfs.{remote}.searchall", "true")
        else:
            add_fallback_remote = False

    if add_fallback_remote and not make_utils.git_remote_exist(args.git_command, fallback_remote):
        print_stage("Adding Git LFS fallback remote")
        print("Used to fetch files from projects.blender.org if missing.")

        url = "https://projects.blender.org/blender/blender.git"
        push_url = "no_push"
        make_utils.git_add_remote(args.git_command, fallback_remote, url, push_url)

        # Fetch potentially missing files.
        call((args.git_command, "lfs", "fetch", fallback_remote))


def main() -> int:
    args = parse_arguments()

    blender_skip_msg = ""
    libraries_skip_msg = ""
    submodules_skip_msg = ""

    blender_version = make_utils. parse_blender_version()
    if blender_version.cycle != 'alpha':
        major = blender_version.version // 100
        minor = blender_version.version % 100
        branch = f"blender-v{major}.{minor}-release"
    else:
        branch = 'main'

    # Submodules and precompiled libraries require Git LFS.
    ensure_git_lfs(args)

    switch_blender_git_remotes(args)

    if args.prune_destructive:
        prune_stale_files(args)

    if not args.no_lfs_fallback:
        lfs_fallback_setup(args)

    if not args.no_blender:
        blender_skip_msg = git_update_skip(args)
        if not blender_skip_msg:
            blender_skip_msg = blender_update(args)
        if blender_skip_msg:
            blender_skip_msg = "Blender repository skipped: " + blender_skip_msg + "\n"
        blender_lfs_update(args)

    if not args.no_libraries:
        libraries_skip_msg += initialize_precompiled_libraries(args)
        libraries_skip_msg += submodules_lib_update(args, branch)

    # Report any skipped repositories at the end, so it's not as easy to miss.
    skip_msg = blender_skip_msg + libraries_skip_msg + submodules_skip_msg
    if skip_msg:
        print_stage("Update finished with the following messages")
        print(skip_msg.strip())

    if args.use_tests:
        print()
        print('NOTE: --use-tests is a deprecated command line argument, kept for compatibility purposes.')

    if args.no_submodules:
        print()
        print('NOTE: --no-submodules is a deprecated command line argument, kept for compatibility purposes.')

    if args.use_linux_libraries:
        print()
        print('NOTE: --use-linux-libraries is a deprecated command line argument, kept for compatibility purposes.')

    # For failed submodule update we throw an error, since not having correct
    # submodules can make Blender throw errors.
    # For Blender itself we don't and consider "make update" to be a command
    # you can use while working on uncommitted code.
    if submodules_skip_msg:
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
