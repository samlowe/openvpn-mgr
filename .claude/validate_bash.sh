#!/bin/bash
# Validate Bash commands before execution

# Print one ls operand per line; use '.' when listing the current directory.
# Tokens that look like shell redirections (e.g. 2>/dev/null, >&2) are skipped
# unless they appear after 'ls --' (operands only).
_collect_ls_operands_from_args() {
    local args="$1"
    local -a toks
    read -ra toks <<< "$args"
    local endopts=false
    local seen_op=false
    local -a ops=()
    local tok
    # EREs in variables so '>' is not parsed as shell redirection inside [[ =~ ... ]]
    local re_fd_out='^[[:digit:]]*>'
    local re_fd_in='^[[:digit:]]*<'
    local re_amp='^&>'

    for tok in "${toks[@]}"; do
        if $endopts; then
            ops+=("$tok")
            continue
        fi
        if [[ "$tok" =~ $re_fd_out ]] || [[ "$tok" =~ $re_fd_in ]] || [[ "$tok" =~ $re_amp ]]; then
            continue
        fi
        if [[ "$tok" == "--" ]]; then
            endopts=true
            continue
        fi
        if ! $seen_op; then
            if [[ "$tok" == -* ]]; then
                continue
            fi
            seen_op=true
        elif [[ "$tok" == -* ]] && { [[ "$tok" == -[[:alnum:]]+ ]] || [[ "$tok" == --* ]]; }; then
            continue
        fi
        ops+=("$tok")
    done

    if [ ${#ops[@]} -eq 0 ]; then
        printf '%s\n' '.'
    else
        printf '%s\n' "${ops[@]}"
    fi
}

# Return 0 if operand resolves to a path under PROJECT_ROOT; else print to stderr and return 2.
_validate_ls_operand_under_project_root() {
    local operand="$1"
    local current_dir="$2"
    local project_root="$3"
    local project_root_norm="$4"
    local rel_display path target_path target_norm

    path="$operand"
    if [[ "$path" == "~" ]]; then
        path="$HOME"
    elif [[ "$path" == '~/'* ]]; then
        path="${HOME}/${path:2}"
    fi
    if [ "${path#/}" != "$path" ]; then
        target_path="$path"
    else
        target_path="${current_dir%/}/$path"
    fi

    if [ ! -e "$target_path" ]; then
        rel_display=$(realpath --relative-to="$project_root" "$current_dir" 2>/dev/null || echo "$current_dir")
        if [ "$current_dir" = "$project_root" ]; then
            rel_display="."
        fi
        echo "ls target does not exist: $operand (you are in: $rel_display relative to project root)" >&2
        return 2
    fi

    target_norm=$(realpath "$target_path")
    if [ "$target_norm" != "$project_root_norm" ] && [ "${target_norm#$project_root_norm/}" = "$target_norm" ]; then
        rel_display=$(realpath --relative-to="$project_root" "$current_dir" 2>/dev/null || echo "$current_dir")
        if [ "$current_dir" = "$project_root" ]; then
            rel_display="."
        fi
        echo "ls outside the project tree is not allowed: $target_norm (you are in: $rel_display relative to project root)" >&2
        return 2
    fi
    return 0
}

# Relative path from project root for error messages; prints "." at project root.
_rel_display_under_project() {
    local project_root="$1"
    local current_dir="$2"
    local rel_display

    rel_display=$(realpath --relative-to="$project_root" "$current_dir" 2>/dev/null || echo "$current_dir")
    if [ "$current_dir" = "$project_root" ]; then
        rel_display="."
    fi
    echo "$rel_display"
}

# Return 0 when target_norm is project root or a path under it.
_is_under_project_root() {
    local target_norm="$1"
    local project_root_norm="$2"

    [ "$target_norm" = "$project_root_norm" ] || [ "${target_norm#$project_root_norm/}" != "$target_norm" ]
}

# Strip quotes, comments, and trailing redirections from a cd operand.
_normalize_cd_operand() {
    local raw="$1"

    echo "$raw" | sed -E 's/[[:space:]]*#.*$//' | sed -E 's/[[:space:]]*(2>|>|>>|&>).*$//' | sed -E "s/^['\"]|['\"]$//g" | xargs
}

# Split a command on &&, ||, ;, and |.
_split_command_segments() {
    local cmd="$1"

    echo "$cmd" | sed -E 's/[[:space:]]*(&&|\|\||;|\|)[[:space:]]*/\n/g'
}

# Trim leading and trailing whitespace.
_trim_segment() {
    local segment="$1"

    segment="${segment#"${segment%%[![:space:]]*}"}"
    segment="${segment%"${segment##*[![:space:]]}"}"
    echo "$segment"
}

# Return 0 when segment is cd <path>; print the normalized operand on stdout.
_parse_cd_segment_operand() {
    local segment="$1"
    local trimmed operand

    trimmed=$(_trim_segment "$segment")
    if [[ "$trimmed" =~ ^cd[[:space:]]*(.+)$ ]]; then
        operand=$(_normalize_cd_operand "${BASH_REMATCH[1]}")
        if [ -n "$operand" ]; then
            echo "$operand"
            return 0
        fi
    fi
    return 1
}

# Return 0 when a short-option token includes ripgrep's replace flag (-r).
_rg_short_flag_has_replace() {
    local flag="${1#-}"

    [[ "$flag" == *r* ]]
}

# Return 0 when a command segment invokes rg with -r/--replace.
_segment_rg_uses_replace_flag() {
    local segment="$1"
    local trimmed in_rg=false after_dd=false tok
    local -a toks

    trimmed=$(_trim_segment "$segment")
    read -ra toks <<< "$trimmed"
    for tok in "${toks[@]}"; do
        if [[ "$tok" =~ ^RG_REPLACE= ]]; then
            continue
        fi
        if $after_dd; then
            return 1
        fi
        if ! $in_rg; then
            if [[ "$tok" == "rg" ]] || [[ "$tok" == */rg ]]; then
                in_rg=true
            fi
            continue
        fi
        if [[ "$tok" == "--" ]]; then
            after_dd=true
            continue
        fi
        if [[ "$tok" == --replace ]] || [[ "$tok" == --replace=* ]]; then
            return 0
        fi
        if [[ "$tok" == -* ]] && [[ "$tok" != --* ]]; then
            if _rg_short_flag_has_replace "$tok"; then
                return 0
            fi
        fi
    done
    return 1
}

# Block rg -r/--replace unless RG_REPLACE is set (inline or in the environment).
_validate_rg_replace_flag() {
    local cmd="$1"
    local segment trimmed

    if [ -n "${RG_REPLACE:-}" ]; then
        return 0
    fi

    while IFS= read -r segment; do
        [ -n "$segment" ] || continue
        trimmed=$(_trim_segment "$segment")
        if [[ "$trimmed" =~ (^|[[:space:]])RG_REPLACE= ]]; then
            continue
        fi
        if _segment_rg_uses_replace_flag "$trimmed"; then
            echo "with rg the -r flag means replace, not recursive. rg (unlike grep) recurses by default. Do not use rg -r for searches. If you really want the replace flag use \`RG_REPLACE=1 rg -r ...\`" >&2
            return 2
        fi
    done < <(_split_command_segments "$cmd")

    return 0
}

# Join non-cd command segments so Check 10 can still block ../ in other commands.
_cmd_remaining_after_removing_cd_segments() {
    local cmd="$1"
    local -a remaining=()
    local segment operand

    while IFS= read -r segment; do
        [ -n "$segment" ] || continue
        if _parse_cd_segment_operand "$segment" >/dev/null; then
            continue
        fi
        remaining+=("$(_trim_segment "$segment")")
    done < <(_split_command_segments "$cmd")

    if [ ${#remaining[@]} -eq 0 ]; then
        echo ""
    else
        local IFS=' '
        echo "${remaining[*]}"
    fi
}

# Validate every cd segment in a command chain; simulate cwd across segments.
_validate_cd_in_command() {
    local cmd="$1"
    local script_dir project_root project_root_norm simulated_cwd segment operand
    local target_path target_norm rel_display

    script_dir=$(dirname "$(readlink -f "$0")")
    project_root=$(dirname "$script_dir")
    project_root_norm=$(realpath "$project_root")
    simulated_cwd=$(realpath "$PWD")

    while IFS= read -r segment; do
        [ -n "$segment" ] || continue
        if ! operand=$(_parse_cd_segment_operand "$segment"); then
            continue
        fi

        if [[ "$operand" == "~" ]]; then
            operand="$HOME"
        elif [[ "$operand" == '~/'* ]]; then
            operand="${HOME}/${operand:2}"
        fi

        if [ "${operand#/}" != "$operand" ]; then
            target_path="$operand"
        else
            target_path="${simulated_cwd%/}/$operand"
        fi

        rel_display=$(_rel_display_under_project "$project_root" "$simulated_cwd")

        if [ "$simulated_cwd" = "$project_root_norm" ] && [[ "$operand" =~ ^\.\.(/|$) ]]; then
            echo "cd .. is not allowed - you are in the project root" >&2
            return 2
        fi

        if [ ! -d "$target_path" ]; then
            echo "cd failed - folder does not exist: $operand (you are in: $rel_display relative to project root)" >&2
            return 2
        fi

        target_norm=$(realpath "$target_path")
        if [ "$target_norm" = "$simulated_cwd" ]; then
            echo "no need to cd (you are in that folder already)" >&2
            return 2
        fi
        if ! _is_under_project_root "$target_norm" "$project_root_norm"; then
            echo "cd outside the project tree is not allowed (you are in: $rel_display relative to project root)" >&2
            return 2
        fi

        simulated_cwd="$target_norm"
    done < <(_split_command_segments "$cmd")

    return 0
}

read -r cmd

# Check 1: Prefer make test over running the test binary directly
if echo "$cmd" | grep -qE '^\s*\./test_core\s*$'; then
    echo "Use 'make test' instead" >&2
    exit 2
fi

# Check 2: Check to not use run commands using bash
if echo "$cmd" | grep -qE "^\s*bash\s+[a-zA-Z0-9_-]+\.sh"; then
    echo "Run the script without the bash prefix" >&2
    exit 2
fi


# Check 3: Prevent grep usage (use ripgrep instead)
if echo "$cmd" | grep -q "^grep "; then
    echo "use ripgrep (rg) instead of grep (but no rg -r for searching - see AGENTS.md)" >&2
    exit 2
fi

# Check 3b: Block rg -r/--replace unless RG_REPLACE is set (agents confuse it with grep -r).
if echo "$cmd" | grep -qE '\brg\b'; then
    if ! _validate_rg_replace_flag "$cmd"; then
        exit 2
    fi
fi

# Check 4: Validate cd segments (including chains and .. paths) stay within the project tree.
if echo "$cmd" | grep -qE '\bcd\b'; then
    if ! _validate_cd_in_command "$cmd"; then
        exit 2
    fi
fi

# Check 6: ls — allow any git ls-* (match git ls-… anywhere so e.g. `&& git ls-files` is not blocked by \bls\b); allow leading ls only when operands resolve under PROJECT_ROOT
if echo "$cmd" | grep -qE '\bgit\s+ls(-|[[:space:]]|$)'; then
    :
elif echo "$cmd" | grep -q "\bls\b"; then
    if echo "$cmd" | grep -qE '^[[:space:]]*ls([[:space:]]|$)'; then
        SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
        PROJECT_ROOT=$(dirname "$SCRIPT_DIR")
        CURRENT_DIR="$PWD"
        PROJECT_ROOT_NORM=$(realpath "$PROJECT_ROOT")

        cmd_for_ls=$(echo "$cmd" | sed -E 's/[[:space:]]*&&.*$//; s/[[:space:]]*\|\|.*$//; s/[;&|].*$//')
        args=$(echo "$cmd_for_ls" | sed -E 's/^[[:space:]]*ls[[:space:]]*//')

        while IFS= read -r operand; do
            [ -n "$operand" ] || continue
            if ! _validate_ls_operand_under_project_root "$operand" "$CURRENT_DIR" "$PROJECT_ROOT" "$PROJECT_ROOT_NORM"; then
                exit 2
            fi
        done < <(_collect_ls_operands_from_args "$args")
    else
        SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
        PROJECT_ROOT=$(dirname "$SCRIPT_DIR")
        CURRENT_DIR="$PWD"
        RELATIVE_PATH=$(realpath --relative-to="$PROJECT_ROOT" "$CURRENT_DIR" 2>/dev/null || echo "$CURRENT_DIR")
        if [ "$CURRENT_DIR" = "$PROJECT_ROOT" ]; then
            RELATIVE_PATH="."
        fi
        echo "ls is only allowed as the leading command, or use git ls-*. You are in: $RELATIVE_PATH relative to project root" >&2
        exit 2
    fi
fi

# Check 8: Prevent absolute home directory paths
if echo "$cmd" | grep -qE "/home/[A-Za-z]+/"; then
    echo "use relative paths" >&2
    exit 2
fi

# Check 9: Prevent pwd command
if echo "$cmd" | grep -qE "^\s*pwd\s*$|^\s*pwd\s+"; then
    # Get project root (parent of .claude directory where this script is located)
    SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
    PROJECT_ROOT=$(dirname "$SCRIPT_DIR")
    
    # Get current directory and calculate relative path
    CURRENT_DIR="$PWD"
    RELATIVE_PATH=$(realpath --relative-to="$PROJECT_ROOT" "$CURRENT_DIR" 2>/dev/null || echo "$CURRENT_DIR")
    
    # If we're at the project root, show "."
    if [ "$CURRENT_DIR" = "$PROJECT_ROOT" ]; then
        RELATIVE_PATH="."
    fi
    
    echo "pwd command is not allowed. Use relative paths - you are in: $RELATIVE_PATH (relative to project root)" >&2
    exit 2
fi

# Check 10: Prevent ../ in non-cd command segments (cd ../ paths are validated in Check 4).
if echo "$cmd" | grep -q '\.\./'; then
    remaining=$(_cmd_remaining_after_removing_cd_segments "$cmd")
    if echo "$remaining" | grep -q '\.\./'; then
        SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
        PROJECT_ROOT=$(dirname "$SCRIPT_DIR")
        CURRENT_DIR="$PWD"
        RELATIVE_PATH=$(_rel_display_under_project "$PROJECT_ROOT" "$CURRENT_DIR")
        echo "cd to the correct folder first - you are in: $RELATIVE_PATH (relative to project root)" >&2
        exit 2
    fi
fi

# Check 11: Prevent git commit, push, merge, and rebase commands
if echo "$cmd" | grep -qE "\bgit\s+(add|reset|commit|push|merge|rebase)\b"; then
    echo "write.update git commands are not allowed" >&2
    exit 2
fi

exit 0
