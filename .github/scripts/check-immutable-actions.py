#!/usr/bin/env python3

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import sys

try:
    import yaml
    from yaml.nodes import MappingNode, ScalarNode, SequenceNode
except ModuleNotFoundError:
    print(
        "PyYAML is required; install immutable-actions-requirements.lock "
        "with --require-hashes",
        file=sys.stderr,
    )
    raise SystemExit(2)


ACTION_RE = re.compile(
    r"([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+"
    r"(?:/[A-Za-z0-9_.-]+)*)@([0-9a-f]{40})"
)
CONTAINER_RE = re.compile(r"docker://[^@\s]+@sha256:[0-9a-f]{64}")
IMAGE_RE = re.compile(r"[^@\s]+@sha256:[0-9a-f]{64}")
MAX_WORKFLOW_BYTES = 1024 * 1024
MAX_WORKFLOW_FILES = 100
MAX_YAML_NODES = 100000
MAX_CONTRACT_BYTES = 128 * 1024
MAX_PROTECTED_FILE_BYTES = 16 * 1024 * 1024


def semantic_sha256(root):
    memo = {}
    active = set()

    def digest(node):
        identity = id(node)
        if identity in memo:
            return memo[identity]
        if identity in active:
            raise ValueError("recursive YAML graph is forbidden")
        active.add(identity)

        fields = []
        if isinstance(node, ScalarNode):
            kind = b"scalar"
            fields = [node.tag.encode("utf-8"), node.value.encode("utf-8")]
        elif isinstance(node, SequenceNode):
            kind = b"sequence"
            fields = [node.tag.encode("utf-8")]
            fields.extend(digest(value) for value in node.value)
        elif isinstance(node, MappingNode):
            kind = b"mapping"
            fields = [node.tag.encode("utf-8")]
            pairs = sorted(
                (digest(key), digest(value)) for key, value in node.value
            )
            fields.extend(key + value for key, value in pairs)
        else:
            raise ValueError("unsupported YAML node")

        value_hash = hashlib.sha256()
        for field in (kind, *fields):
            value_hash.update(len(field).to_bytes(8, "big"))
            value_hash.update(field)
        active.remove(identity)
        memo[identity] = value_hash.digest()
        return memo[identity]

    return digest(root).hex()


def extract_uses(text, source):
    try:
        documents = list(yaml.compose_all(text, Loader=yaml.SafeLoader))
    except yaml.YAMLError as error:
        mark = getattr(error, "problem_mark", None)
        line = mark.line + 1 if mark is not None else 1
        problem = getattr(error, "problem", None) or str(error)
        raise ValueError(f"{source}:{line}: invalid YAML: {problem}") from error

    documents = [document for document in documents if document is not None]
    if len(documents) != 1:
        raise ValueError(f"{source}: workflow must contain one YAML document")
    root = documents[0]
    if not isinstance(root, MappingNode):
        raise ValueError(f"{source}: workflow root must be a mapping")

    references = []
    states = {}
    node_count = 0

    def line_number(node):
        return node.start_mark.line + 1

    def visit(node):
        nonlocal node_count
        identity = id(node)
        state = states.get(identity)
        if state == "active":
            raise ValueError(
                f"{source}:{line_number(node)}: recursive YAML graph is forbidden"
            )
        if state == "done":
            return

        states[identity] = "active"
        node_count += 1
        if node_count > MAX_YAML_NODES:
            raise ValueError(f"{source}: YAML node limit exceeded")

        if isinstance(node, MappingNode):
            keys = set()
            for key, value in node.value:
                if not isinstance(key, ScalarNode):
                    raise ValueError(
                        f"{source}:{line_number(key)}: mapping keys must be scalars"
                    )
                if key.value == "<<":
                    raise ValueError(
                        f"{source}:{line_number(key)}: YAML merge keys are forbidden"
                    )
                if key.value in keys:
                    raise ValueError(
                        f"{source}:{line_number(key)}: duplicate mapping key: "
                        f"{key.value!r}"
                    )
                keys.add(key.value)
                visit(key)
                if key.value == "uses":
                    if not isinstance(value, ScalarNode) or not value.value:
                        raise ValueError(
                            f"{source}:{line_number(key)}: uses has no scalar value"
                        )
                    references.append((line_number(value), value.value))
                visit(value)
        elif isinstance(node, SequenceNode):
            for value in node.value:
                visit(value)
        elif not isinstance(node, ScalarNode):
            raise ValueError(
                f"{source}:{line_number(node)}: unsupported YAML node"
            )

        states[identity] = "done"

    visit(root)
    return references, extract_container_images(root, source), root


def mapping_entries(node, description):
    if isinstance(node, dict):
        return node
    if not isinstance(node, MappingNode):
        raise ValueError(f"{description} must be a mapping")
    return {key.value: value for key, value in node.value}


def scalar_value(node, description):
    if not isinstance(node, ScalarNode):
        raise ValueError(f"{description} must be a scalar")
    return node.value


def extract_container_images(root, source):
    images = []
    jobs = mapping_entries(root, f"{source}: workflow root").get("jobs")
    if jobs is None:
        return images
    for job_name, job in mapping_entries(jobs, f"{source}: jobs").items():
        job_entries = mapping_entries(job, f"{source}: job {job_name}")
        container = job_entries.get("container")
        if container is not None:
            if isinstance(container, MappingNode):
                image = mapping_entries(
                    container, f"{source}: job {job_name} container"
                ).get("image")
                if image is None:
                    raise ValueError(
                        f"{source}: job {job_name} container has no image"
                    )
            else:
                image = container
            images.append(
                (
                    image.start_mark.line + 1,
                    scalar_value(
                        image, f"{source}: job {job_name} container image"
                    ),
                )
            )

        services = job_entries.get("services")
        if services is None:
            continue
        for service_name, service in mapping_entries(
            services, f"{source}: job {job_name} services"
        ).items():
            service_entries = mapping_entries(
                service,
                f"{source}: job {job_name} service {service_name}",
            )
            image = service_entries.get("image")
            if image is None:
                raise ValueError(
                    f"{source}: job {job_name} service {service_name} "
                    "has no image"
                )
            images.append(
                (
                    image.start_mark.line + 1,
                    scalar_value(
                        image,
                        f"{source}: job {job_name} service {service_name} image",
                    ),
                )
            )
    return images


def load_allowlist(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError("action allowlist is unavailable or unsafe")
    entries = {}
    previous = None
    for line_number, raw in enumerate(
        path.read_text(encoding="ascii").splitlines(), start=1
    ):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 2 or not re.fullmatch(
            r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*",
            fields[0],
        ) or not re.fullmatch(r"[0-9a-f]{40}", fields[1]):
            raise ValueError(f"invalid action allowlist entry at {path}:{line_number}")
        if fields[0] in entries or (previous is not None and fields[0] <= previous):
            raise ValueError("action allowlist entries must be unique and sorted")
        entries[fields[0]] = fields[1]
        previous = fields[0]
    if not entries:
        raise ValueError("action allowlist is empty")
    return entries


def is_local_action(reference):
    if not reference.startswith("./"):
        return False
    path = PurePosixPath(reference[2:])
    return bool(path.parts) and not any(
        part in {"", ".", ".."} for part in path.parts
    )


def validate_reference(reference, allowlist):
    if is_local_action(reference):
        raise ValueError("local action is not recursively verified")
    if CONTAINER_RE.fullmatch(reference):
        return
    match = ACTION_RE.fullmatch(reference)
    if not match:
        raise ValueError("reference is not immutable")
    action, revision = match.groups()
    if allowlist.get(action) != revision:
        raise ValueError("action repository and revision are not allowlisted")


def validate_container_image(image):
    if not IMAGE_RE.fullmatch(image):
        raise ValueError("container image is not immutable")


def workflow_files(path):
    if path.is_symlink():
        raise ValueError("workflow path must not be a symlink")
    if path.is_file():
        return [path]
    if not path.is_dir():
        raise ValueError("workflow path is unavailable")
    candidates = sorted(path.rglob("*"))
    unsafe = [candidate for candidate in candidates if candidate.is_symlink()]
    if unsafe:
        raise ValueError(f"unsafe workflow path entry: {unsafe[0]}")
    files = [
        candidate
        for candidate in candidates
        if candidate.suffix in {".yml", ".yaml"} and candidate.is_file()
    ]
    if not files:
        raise ValueError("no workflow files were found")
    if len(files) > MAX_WORKFLOW_FILES:
        raise ValueError("workflow file limit exceeded")
    return files


def required_entry(mapping, name, description):
    entries = mapping_entries(mapping, description)
    if name not in entries:
        raise ValueError(f"{description} is missing {name!r}")
    return entries[name]


def scalar_sequence(node, description):
    if not isinstance(node, SequenceNode):
        raise ValueError(f"{description} must be a sequence")
    return [
        scalar_value(value, f"{description} item") for value in node.value
    ]


def require_scalar(mapping, name, expected, description):
    actual = scalar_value(
        required_entry(mapping, name, description),
        f"{description}.{name}",
    )
    if actual != expected:
        raise ValueError(
            f"{description}.{name} must be {expected!r}, got {actual!r}"
        )


def require_permissions(node, expected, description):
    permissions = {
        name: scalar_value(value, f"{description}.{name}")
        for name, value in mapping_entries(node, description).items()
    }
    if permissions != expected:
        raise ValueError(
            f"{description} must be exactly {expected!r}, got {permissions!r}"
        )


def require_needs(job, expected, description):
    needs = required_entry(job, "needs", description)
    if isinstance(needs, ScalarNode):
        observed = [needs.value]
    else:
        observed = scalar_sequence(needs, f"{description}.needs")
    if set(observed) != set(expected) or len(observed) != len(expected):
        raise ValueError(
            f"{description}.needs must be exactly {sorted(expected)!r}"
        )


def workflow_steps(job, description):
    steps_node = required_entry(job, "steps", description)
    if not isinstance(steps_node, SequenceNode):
        raise ValueError(f"{description}.steps must be a sequence")
    steps = []
    names = {}
    for index, step in enumerate(steps_node.value, start=1):
        entries = mapping_entries(step, f"{description}.steps[{index}]")
        steps.append(entries)
        name = entries.get("name")
        if name is None:
            continue
        name = scalar_value(name, f"{description}.steps[{index}].name")
        if name in names:
            raise ValueError(f"{description} has duplicate step name {name!r}")
        names[name] = entries
    return steps, names


def require_step_names(job, expected, description, exact=False):
    steps, names = workflow_steps(job, description)
    missing = [name for name in expected if name not in names]
    if missing:
        raise ValueError(
            f"{description} is missing required steps: {', '.join(missing)}"
        )
    if exact and list(names) != list(expected):
        raise ValueError(
            f"{description} step names do not match the trusted contract"
        )
    return steps, names


def require_with(step, expected, description, exact=False):
    values = mapping_entries(
        required_entry(step, "with", description), f"{description}.with"
    )
    observed = {
        name: scalar_value(value, f"{description}.with.{name}")
        for name, value in values.items()
    }
    if any(observed.get(name) != value for name, value in expected.items()):
        raise ValueError(f"{description}.with weakens a required setting")
    if exact and observed != expected:
        raise ValueError(f"{description}.with has unexpected settings")


def require_environment(step, expected, description):
    environment = mapping_entries(
        required_entry(step, "env", description), f"{description}.env"
    )
    observed = {
        name: scalar_value(value, f"{description}.env.{name}")
        for name, value in environment.items()
    }
    if observed != expected:
        raise ValueError(f"{description}.env does not match the trusted contract")


def require_run_fragments(step, fragments, description):
    script = scalar_value(
        required_entry(step, "run", description), f"{description}.run"
    )
    for fragment in fragments:
        if fragment not in script:
            raise ValueError(
                f"{description}.run is missing required behavior: {fragment}"
            )
    return script


def require_run_sha256(step, expected, description):
    script = scalar_value(
        required_entry(step, "run", description), f"{description}.run"
    )
    actual = hashlib.sha256(script.encode("utf-8")).hexdigest()
    if actual != expected:
        raise ValueError(f"{description}.run changed from the trusted contract")


def load_contract(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError("workflow policy contract is unavailable or unsafe")
    if path.stat().st_size > MAX_CONTRACT_BYTES:
        raise ValueError("workflow policy contract is too large")
    document = json.loads(path.read_text(encoding="ascii"))
    if not isinstance(document, dict) or set(document) != {
        "format", "policy_workflow", "build_workflows", "protected_files"
    }:
        raise ValueError("workflow policy contract has an invalid schema")
    if document["format"] != "goldencheetah-workflow-policy-contract-2":
        raise ValueError("workflow policy contract has an invalid format")
    policy = document["policy_workflow"]
    builds = document["build_workflows"]
    protected_files = document["protected_files"]
    if not isinstance(policy, dict) or set(policy) != {
        "base_branch", "file", "job", "pull_request_types", "run_sha256",
        "semantic_sha256", "status_context"
    } or not isinstance(builds, dict) or not builds:
        raise ValueError("workflow policy contract has an invalid schema")
    if policy["base_branch"] != "master":
        raise ValueError("workflow policy contract has an invalid base branch")
    if not isinstance(policy["pull_request_types"], list) or not all(
        isinstance(value, str) and value for value in policy["pull_request_types"]
    ):
        raise ValueError("workflow policy contract has invalid PR event types")
    if not isinstance(policy["run_sha256"], dict) or any(
        not isinstance(name, str)
        or not isinstance(digest, str)
        or not re.fullmatch(r"[0-9a-f]{64}", digest)
        for name, digest in policy["run_sha256"].items()
    ):
        raise ValueError("workflow policy contract has invalid policy run hashes")
    if not isinstance(policy["semantic_sha256"], list) or not policy[
        "semantic_sha256"
    ] or any(
        not isinstance(digest, str)
        or not re.fullmatch(r"[0-9a-f]{64}", digest)
        for digest in policy["semantic_sha256"]
    ):
        raise ValueError("workflow policy contract has invalid semantic hashes")
    for filename, build in builds.items():
        if not isinstance(filename, str) or not filename.endswith((".yml", ".yaml")):
            raise ValueError("workflow policy contract has an invalid filename")
        if not isinstance(build, dict) or set(build) != {
            "candidate_jobs", "candidate_runs_on", "candidate_step_count",
            "path_filters", "run_sha256", "semantic_sha256", "status_context",
            "trusted_run_sha256"
        } or not isinstance(build["candidate_jobs"], dict) or not isinstance(
            build["run_sha256"], dict
        ) or not isinstance(build["trusted_run_sha256"], dict) or not isinstance(
            build["candidate_runs_on"], dict
        ) or not isinstance(build["candidate_step_count"], dict):
            raise ValueError("workflow policy contract has an invalid build entry")
        if not isinstance(build["semantic_sha256"], list) or not build[
            "semantic_sha256"
        ] or any(
            not isinstance(digest, str)
            or not re.fullmatch(r"[0-9a-f]{64}", digest)
            for digest in build["semantic_sha256"]
        ):
            raise ValueError("workflow policy contract has invalid semantic hashes")
        if not isinstance(build["path_filters"], list) or not build[
            "path_filters"
        ] or any(
            not isinstance(pattern, str)
            or not pattern
            or "\n" in pattern
            or pattern.startswith("/")
            or ".." in PurePosixPath(pattern).parts
            for pattern in build["path_filters"]
        ):
            raise ValueError("workflow policy contract has invalid path filters")
        for job, steps in build["candidate_jobs"].items():
            if not isinstance(job, str) or not isinstance(steps, list) or not all(
                isinstance(step, str) and step for step in steps
            ):
                raise ValueError(
                    "workflow policy contract has invalid candidate steps"
                )
        if set(build["run_sha256"]) != set(build["candidate_jobs"]):
            raise ValueError("workflow policy contract has incomplete run hashes")
        if set(build["candidate_runs_on"]) != set(build["candidate_jobs"]) or any(
            not isinstance(value, str) or not value
            for value in build["candidate_runs_on"].values()
        ):
            raise ValueError("workflow policy contract has invalid candidate runners")
        if set(build["candidate_step_count"]) != set(build["candidate_jobs"]) or any(
            not isinstance(value, int) or value < 1
            for value in build["candidate_step_count"].values()
        ):
            raise ValueError("workflow policy contract has invalid step counts")
        for job, hashes in build["run_sha256"].items():
            if not isinstance(hashes, dict) or any(
                step not in build["candidate_jobs"][job]
                or not isinstance(digest, str)
                or not re.fullmatch(r"[0-9a-f]{64}", digest)
                for step, digest in hashes.items()
            ):
                raise ValueError("workflow policy contract has invalid run hashes")
        if set(build["trusted_run_sha256"]) != {
            "validated-candidate", "report-pending", "report-final"
        } or any(
            not isinstance(digest, str)
            or not re.fullmatch(r"[0-9a-f]{64}", digest)
            for digest in build["trusted_run_sha256"].values()
        ):
            raise ValueError("workflow policy contract has invalid trusted hashes")
    if not isinstance(protected_files, dict) or not protected_files:
        raise ValueError("workflow policy contract has no protected files")
    if list(protected_files) != sorted(protected_files):
        raise ValueError("protected CI files must be sorted")
    for relative, digest in protected_files.items():
        path = PurePosixPath(relative)
        if (
            not isinstance(relative, str)
            or not relative
            or path.is_absolute()
            or any(part in {"", ".", ".."} for part in path.parts)
            or not isinstance(digest, str)
            or not re.fullmatch(r"[0-9a-f]{64}", digest)
        ):
            raise ValueError("workflow policy contract has an invalid protected file")
    return document


def enforce_protected_files(repository_root, protected_files):
    if repository_root.is_symlink() or not repository_root.is_dir():
        raise ValueError("candidate repository root is unavailable or unsafe")
    for relative, expected in protected_files.items():
        candidate = repository_root.joinpath(*PurePosixPath(relative).parts)
        current = candidate
        while current != repository_root:
            if current.is_symlink():
                raise ValueError(
                    f"protected CI file is unavailable or unsafe: {relative}"
                )
            current = current.parent
        if not candidate.is_file():
            raise ValueError(
                f"protected CI file is unavailable or unsafe: {relative}"
            )
        if candidate.stat().st_size > MAX_PROTECTED_FILE_BYTES:
            raise ValueError(f"protected CI file is too large: {relative}")
        actual = hashlib.sha256(candidate.read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f"protected CI file changed: {relative}")


def enforce_policy_workflow(
    root, policy, checkout_reference, protected_files
):
    description = "workflow-policy.yml"
    if semantic_sha256(root) not in policy["semantic_sha256"]:
        raise ValueError(f"{description} semantic schema changed")
    require_scalar(root, "name", "Workflow policy", description)
    events = required_entry(root, "on", description)
    event_entries = mapping_entries(events, f"{description}.on")
    if set(event_entries) != {"pull_request_target"}:
        raise ValueError(
            f"{description}.on must contain only pull_request_target"
        )
    pull_request = event_entries["pull_request_target"]
    pull_request_entries = mapping_entries(
        pull_request, f"{description}.on.pull_request_target"
    )
    if set(pull_request_entries) != {"branches", "types"}:
        raise ValueError(
            f"{description} pull_request_target schema is not trusted"
        )
    branches = scalar_sequence(
        pull_request_entries["branches"],
        f"{description}.on.pull_request_target.branches",
    )
    if branches != [policy["base_branch"]]:
        raise ValueError(f"{description} pull request base branch was weakened")
    types = scalar_sequence(
        pull_request_entries["types"],
        f"{description}.on.pull_request_target.types",
    )
    if types != policy["pull_request_types"]:
        raise ValueError(f"{description} pull request event types were weakened")

    require_permissions(
        required_entry(root, "permissions", description),
        {"contents": "read", "statuses": "write"},
        f"{description}.permissions",
    )
    jobs = required_entry(root, "jobs", description)
    job_entries = mapping_entries(jobs, f"{description}.jobs")
    if set(job_entries) != {policy["job"]}:
        raise ValueError(f"{description} jobs do not match the trusted contract")
    job = job_entries[policy["job"]]
    require_scalar(
        job,
        "if",
        "${{ github.event.repository.full_name == github.repository && github.event.pull_request.base.repo.full_name == github.repository && github.event.pull_request.base.ref == 'master' }}",
        f"{description}.{policy['job']}",
    )
    require_scalar(job, "runs-on", "ubuntu-24.04", f"{description}.{policy['job']}")
    require_scalar(job, "timeout-minutes", "5", f"{description}.{policy['job']}")
    expected_steps = (
        "Validate pull request identity",
        "Mark candidate policy pending",
        "Checkout trusted policy source",
        "Install hash-locked YAML parser",
        "Checkout candidate workflows as data",
        "Validate candidate workflow data",
        "Publish candidate policy result",
    )
    _, steps = require_step_names(
        job, expected_steps, f"{description}.{policy['job']}", exact=True
    )
    expected_run_steps = {
        expected_steps[0], expected_steps[1], expected_steps[3],
        expected_steps[5], expected_steps[6]
    }
    if set(policy["run_sha256"]) != expected_run_steps:
        raise ValueError(f"{description} run hash contract is incomplete")
    for step_name, digest in policy["run_sha256"].items():
        require_run_sha256(
            steps[step_name], digest, f"{description} {step_name}"
        )

    identity = steps[expected_steps[0]]
    require_environment(
        identity,
        {
            "EVENT_REPOSITORY": "${{ github.event.repository.full_name }}",
            "BASE_REPOSITORY": "${{ github.event.pull_request.base.repo.full_name }}",
            "BASE_REF": "${{ github.event.pull_request.base.ref }}",
            "BASE_SHA": "${{ github.event.pull_request.base.sha }}",
            "HEAD_REPOSITORY": "${{ github.event.pull_request.head.repo.full_name }}",
            "HEAD_SHA": "${{ github.event.pull_request.head.sha }}",
            "PULL_NUMBER": "${{ github.event.pull_request.number }}",
        },
        f"{description} event identity",
    )
    identity_script = require_run_fragments(
        identity,
        (
            '[ "$EVENT_REPOSITORY" = "$GITHUB_REPOSITORY" ]',
            '[ "$BASE_REPOSITORY" = "$GITHUB_REPOSITORY" ]',
            '[ "$BASE_REF" = master ]',
            '[[ "$BASE_SHA" =~ ^[0-9a-f]{40}$ ]]',
            '[[ "$HEAD_REPOSITORY" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]]',
            '[[ "$HEAD_SHA" =~ ^[0-9a-f]{40}$ ]]',
            '[[ "$PULL_NUMBER" =~ ^[1-9][0-9]*$ ]]',
        ),
        f"{description} event identity",
    )
    if "candidate/" in identity_script:
        raise ValueError(f"{description} event identity executes candidate data")

    pending = steps[expected_steps[1]]
    require_environment(
        pending,
        {
            "GH_TOKEN": "${{ github.token }}",
            "HEAD_SHA": "${{ github.event.pull_request.head.sha }}",
        },
        f"{description} pending status",
    )
    pending_script = require_run_fragments(
        pending,
        (
            "/statuses/${HEAD_SHA}",
            "state=pending",
            f"context='{policy['status_context']}'",
        ),
        f"{description} pending status",
    )
    if "candidate/" in pending_script:
        raise ValueError(f"{description} pending reporter executes candidate data")

    trusted_checkout = steps[expected_steps[2]]
    require_scalar(
        trusted_checkout, "uses", checkout_reference,
        f"{description} trusted checkout",
    )
    require_with(
        trusted_checkout,
        {
            "ref": "${{ github.event.pull_request.base.sha }}",
            "path": "trusted-policy",
            "persist-credentials": "false",
        },
        f"{description} trusted checkout",
        exact=True,
    )

    install_step = steps[expected_steps[3]]
    require_environment(
        install_step,
        {"POLICY_DEPS": "${{ runner.temp }}/workflow-policy-deps"},
        f"{description} parser installation",
    )
    install_script = require_run_fragments(
        install_step,
        (
            "--require-hashes",
            "--only-binary=:all:",
            "trusted-policy/.github/scripts/immutable-actions-requirements.lock",
        ),
        f"{description} parser installation",
    )
    if "candidate/" in install_script:
        raise ValueError(f"{description} parser installation executes candidate data")

    candidate_checkout = steps[expected_steps[4]]
    require_scalar(
        candidate_checkout, "uses", checkout_reference,
        f"{description} candidate checkout",
    )
    sparse_checkout = "\n".join(
        f"/{relative}"
        for relative in sorted({".github/workflows/", *protected_files})
    )
    require_with(
        candidate_checkout,
        {
            "repository": "${{ github.event.pull_request.head.repo.full_name }}",
            "ref": "${{ github.event.pull_request.head.sha }}",
            "path": "candidate",
            "persist-credentials": "false",
            "fetch-depth": "1",
            "sparse-checkout": sparse_checkout,
            "sparse-checkout-cone-mode": "false",
        },
        f"{description} candidate checkout",
        exact=True,
    )

    validation_step = steps[expected_steps[5]]
    require_environment(
        validation_step,
        {
            "PYTHONDONTWRITEBYTECODE": "1",
            "PYTHONPATH": "${{ runner.temp }}/workflow-policy-deps",
        },
        f"{description} validation",
    )
    validation_script = require_run_fragments(
        validation_step,
        (
            "python3 trusted-policy/.github/scripts/check-immutable-actions.py",
            "--workflows candidate/.github/workflows",
            "--repository-root candidate",
            "--allowlist trusted-policy/.github/actions.lock",
            "--enforce-policy",
        ),
        f"{description} validation",
    )
    candidate_lines = [
        line for line in validation_script.splitlines() if "candidate" in line
    ]
    if candidate_lines != [
        "  --workflows candidate/.github/workflows \\",
        "  --repository-root candidate \\",
    ]:
        raise ValueError(f"{description} validation executes candidate data")

    final = steps[expected_steps[6]]
    require_scalar(final, "if", "${{ always() }}", f"{description} final status")
    require_environment(
        final,
        {
            "GH_TOKEN": "${{ github.token }}",
            "HEAD_SHA": "${{ github.event.pull_request.head.sha }}",
            "POLICY_STATE": "${{ steps.validate.outcome == 'success' && 'success' || 'failure' }}",
        },
        f"{description} final status",
    )
    final_script = require_run_fragments(
        final,
        (
            "/statuses/${HEAD_SHA}",
            '--raw-field state="$POLICY_STATE"',
            f"context='{policy['status_context']}'",
        ),
        f"{description} final status",
    )
    if "candidate/" in final_script:
        raise ValueError(f"{description} final reporter executes candidate data")


def enforce_trusted_macos_release(job, checkout_reference):
    description = "ci.yml.trusted-macos-release"
    require_scalar(job, "runs-on", "macos-latest", description)
    require_scalar(job, "timeout-minutes", "90", description)
    require_scalar(
        job,
        "if",
        "${{ github.event_name == 'push' && github.ref == "
        "'refs/heads/master' && contains(github.event.head_commit.message, "
        "'[publish binaries]') }}",
        description,
    )
    require_needs(job, ("validated-candidate", "macos"), description)
    require_permissions(
        required_entry(job, "permissions", description),
        {"contents": "write"},
        f"{description}.permissions",
    )
    secret_names = (
        "GC_AZUM_CLIENT_SECRET",
        "GC_CLOUD_DB_APP_NAME",
        "GC_CLOUD_DB_BASIC_AUTH",
        "GC_CLOUD_OPENDATA_SECRET",
        "GC_CYCLINGANALYTICS_CLIENT_SECRET",
        "GC_DROPBOX_CLIENT_SECRET",
        "GC_NOKIA_CLIENT_SECRET",
        "GC_NOLIO_CLIENT_ID",
        "GC_NOLIO_SECRET",
        "GC_POLARFLOW_CLIENT_SECRET",
        "GC_RWGPS_API_KEY",
        "GC_SPORTTRACKS_CLIENT_SECRET",
        "GC_TRAINERDAY_API_KEY",
        "GC_XERT_CLIENT_SECRET",
    )
    require_environment(
        job,
        {name: "${{ secrets." + name + " }}" for name in secret_names},
        f"{description}.environment",
    )

    expected_steps = (
        "Checkout trusted release source",
        "Add Secrets",
        "Build",
        "Test",
        "Upload release dmg",
        "Release",
    )
    steps, named = require_step_names(
        job, expected_steps, description, exact=True
    )
    if len(steps) != len(expected_steps):
        raise ValueError(f"{description} step schema changed")

    checkout = named[expected_steps[0]]
    require_scalar(checkout, "uses", checkout_reference, f"{description}.checkout")
    require_with(
        checkout,
        {
            "repository": "${{ github.repository }}",
            "ref": "${{ github.sha }}",
            "persist-credentials": "false",
        },
        f"{description}.checkout",
        exact=True,
    )

    add_secrets = named[expected_steps[1]]
    require_scalar(add_secrets, "shell", "pwsh", f"{description}.Add Secrets")
    require_scalar(
        add_secrets,
        "run",
        "./util/add_secrets.ps1",
        f"{description}.Add Secrets",
    )
    require_scalar(
        named[expected_steps[2]],
        "run",
        "./.github/scripts/build.sh",
        f"{description}.Build",
    )
    require_run_fragments(
        named[expected_steps[3]],
        (
            "appveyor/macos/run-build-regressions.sh",
            ".github/scripts/run-tests.py --platform macos",
            "GoldenCheetah --version",
            "shasum -a 256",
        ),
        f"{description}.Test",
    )
    require_with(
        named[expected_steps[4]],
        {
            "name": "GoldenCheetah-release",
            "path": "GoldenCheetah_v*_arm64.dmg",
        },
        f"{description}.Upload release dmg",
        exact=True,
    )
    require_with(
        named[expected_steps[5]],
        {
            "tag_name": "snapshot",
            "name": "Snapshot Builds",
            "prerelease": "true",
            "files": (
                "GoldenCheetah_v*_arm64.dmg\n"
                "GCversionMacOS_arm64.txt\n"
            ),
        },
        f"{description}.Release",
        exact=True,
    )


def enforce_build_workflow(root, filename, build, checkout_reference):
    if semantic_sha256(root) not in build["semantic_sha256"]:
        raise ValueError(f"{filename} semantic schema changed")
    events = required_entry(root, "on", filename)
    event_entries = mapping_entries(events, f"{filename}.on")
    if "pull_request" in event_entries or "pull_request_target" in event_entries:
        raise ValueError(f"{filename} may not execute directly for pull requests")
    workflow_run = mapping_entries(
        required_entry(events, "workflow_run", f"{filename}.on"),
        f"{filename}.on.workflow_run",
    )
    if set(workflow_run) != {"workflows", "types"} or scalar_sequence(
        workflow_run["workflows"], f"{filename}.on.workflow_run.workflows"
    ) != ["Workflow policy"] or scalar_sequence(
        workflow_run["types"], f"{filename}.on.workflow_run.types"
    ) != ["completed"]:
        raise ValueError(f"{filename} workflow_run contract was weakened")
    if filename == "ci.yml":
        if set(event_entries) != {"push", "workflow_run"}:
            raise ValueError("ci.yml release events are not trusted")
        push = mapping_entries(event_entries["push"], "ci.yml.on.push")
        if set(push) != {"branches"} or scalar_sequence(
            push["branches"], "ci.yml.on.push.branches"
        ) != ["master"]:
            raise ValueError("ci.yml trusted push branch was weakened")
    require_permissions(
        required_entry(root, "permissions", filename),
        {"contents": "read", "pull-requests": "read"},
        f"{filename}.permissions",
    )

    jobs = required_entry(root, "jobs", filename)
    job_entries = mapping_entries(jobs, f"{filename}.jobs")
    candidate_jobs = build["candidate_jobs"]
    expected_jobs = {
        "validated-candidate", "report-pending", "report-final", *candidate_jobs
    }
    if filename == "ci.yml":
        expected_jobs.add("trusted-macos-release")
    if set(job_entries) != expected_jobs:
        raise ValueError(f"{filename} required jobs were deleted or added")

    resolver = job_entries["validated-candidate"]
    require_scalar(
        resolver, "runs-on", "ubuntu-24.04", f"{filename}.validated-candidate"
    )
    _, resolver_steps = require_step_names(
        resolver,
        ("Resolve trusted candidate identity",),
        f"{filename}.validated-candidate",
        exact=True,
    )
    outputs = mapping_entries(
        required_entry(resolver, "outputs", f"{filename}.validated-candidate"),
        f"{filename}.validated-candidate.outputs",
    )
    expected_outputs = {
        "authorized": "${{ steps.resolve.outputs.authorized }}",
        "repository": "${{ steps.resolve.outputs.repository }}",
        "revision": "${{ steps.resolve.outputs.revision }}",
        "selected": "${{ steps.resolve.outputs.selected }}",
    }
    observed_outputs = {
        name: scalar_value(value, f"{filename}.validated-candidate.outputs.{name}")
        for name, value in outputs.items()
    }
    if observed_outputs != expected_outputs:
        raise ValueError(f"{filename} candidate resolver outputs were weakened")
    require_environment(
        resolver_steps["Resolve trusted candidate identity"],
        {
            "GH_TOKEN": "${{ github.token }}",
            "EVENT_NAME": "${{ github.event_name }}",
            "CURRENT_REPOSITORY": "${{ github.repository }}",
            "CURRENT_REVISION": "${{ github.sha }}",
            "POLICY_EVENT": "${{ github.event.workflow_run.event }}",
            "POLICY_PATH": "${{ github.event.workflow_run.path }}",
            "POLICY_CONCLUSION": "${{ github.event.workflow_run.conclusion }}",
            "POLICY_IDENTITY": "${{ github.event.workflow_run.display_title }}",
            "POLICY_REPOSITORY": "${{ github.event.workflow_run.repository.full_name }}",
            "POLICY_HEAD_BRANCH": "${{ github.event.workflow_run.head_branch }}",
            "POLICY_HEAD_SHA": "${{ github.event.workflow_run.head_sha }}",
            "PATH_FILTERS": "\n".join(build["path_filters"]),
        },
        f"{filename} candidate resolver",
    )
    resolver_script = require_run_fragments(
        resolver_steps["Resolve trusted candidate identity"],
        (
            '[ "$POLICY_EVENT" = pull_request_target ]',
            '[ "$POLICY_PATH" = .github/workflows/workflow-policy.yml ]',
            '[ "$POLICY_CONCLUSION" = success ]',
            '[ "$POLICY_REPOSITORY" = "$CURRENT_REPOSITORY" ]',
            '[ "$POLICY_HEAD_BRANCH" = master ]',
            "workflow-policy:*:*:[0-9a-f][0-9a-f]*",
            '[[ "$pull_number" =~ ^[1-9][0-9]*$ ]]',
            '[[ "$revision" =~ ^[0-9a-f]{40}$ ]]',
            'pulls/${pull_number}/files?per_page=100',
            "selected=false",
            "json.load",
            '>>"$GITHUB_OUTPUT"',
        ),
        f"{filename} candidate resolver",
    )
    if "candidate/" in resolver_script:
        raise ValueError(f"{filename} resolver executes candidate data")
    require_run_sha256(
        resolver_steps["Resolve trusted candidate identity"],
        build["trusted_run_sha256"]["validated-candidate"],
        f"{filename} candidate resolver",
    )

    pending = job_entries["report-pending"]
    require_scalar(
        pending, "runs-on", "ubuntu-24.04", f"{filename}.report-pending"
    )
    require_needs(
        pending, ("validated-candidate",), f"{filename}.report-pending"
    )
    require_scalar(
        pending,
        "if",
        "${{ github.event_name == 'workflow_run' && needs.validated-candidate.outputs.authorized == 'true' && needs.validated-candidate.outputs.selected == 'true' }}",
        f"{filename}.report-pending",
    )
    require_permissions(
        required_entry(pending, "permissions", f"{filename}.report-pending"),
        {"statuses": "write"},
        f"{filename}.report-pending.permissions",
    )
    pending_steps, _ = workflow_steps(pending, f"{filename}.report-pending")
    if len(pending_steps) != 1 or "uses" in pending_steps[0]:
        raise ValueError(f"{filename} pending reporter is not isolated")
    require_environment(
        pending_steps[0],
        {
            "GH_TOKEN": "${{ github.token }}",
            "CANDIDATE_SHA": "${{ needs.validated-candidate.outputs.revision }}",
            "STATUS_CONTEXT": build["status_context"],
        },
        f"{filename} pending reporter",
    )
    require_run_fragments(
        pending_steps[0],
        (
            "/statuses/${CANDIDATE_SHA}",
            "state=pending",
            '--raw-field context="$STATUS_CONTEXT"',
        ),
        f"{filename} pending reporter",
    )
    require_run_sha256(
        pending_steps[0],
        build["trusted_run_sha256"]["report-pending"],
        f"{filename} pending reporter",
    )

    for job_name, required_steps in candidate_jobs.items():
        job = job_entries[job_name]
        job_values = mapping_entries(job, f"{filename}.{job_name}")
        require_scalar(
            job,
            "runs-on",
            build["candidate_runs_on"][job_name],
            f"{filename}.{job_name}",
        )
        permissions = job_values.get("permissions")
        if permissions is not None:
            require_permissions(
                permissions,
                {"contents": "read"},
                f"{filename}.{job_name}.permissions",
            )
        require_needs(
            job,
            ("validated-candidate", "report-pending"),
            f"{filename}.{job_name}",
        )
        require_scalar(
            job,
            "if",
            "${{ always() && needs.validated-candidate.outputs.authorized == 'true' && needs.validated-candidate.outputs.selected == 'true' && (github.event_name != 'workflow_run' || needs.report-pending.result == 'success') }}",
            f"{filename}.{job_name}",
        )
        steps, named_steps = require_step_names(
            job, required_steps, f"{filename}.{job_name}"
        )
        if len(steps) != build["candidate_step_count"][job_name]:
            raise ValueError(
                f"{filename}.{job_name} step schema changed"
            )
        for step_name, expected_digest in build["run_sha256"][job_name].items():
            script = scalar_value(
                required_entry(
                    named_steps[step_name],
                    "run",
                    f"{filename}.{job_name}.{step_name}",
                ),
                f"{filename}.{job_name}.{step_name}.run",
            )
            actual_digest = hashlib.sha256(script.encode("utf-8")).hexdigest()
            if actual_digest != expected_digest:
                raise ValueError(
                    f"{filename}.{job_name}.{step_name} run script changed"
                )
        checkouts = [
            step for step in steps
            if isinstance(step.get("uses"), ScalarNode)
            and step["uses"].value == checkout_reference
        ]
        if len(checkouts) != 1:
            raise ValueError(f"{filename}.{job_name} must checkout once")
        require_with(
            checkouts[0],
            {
                "repository": "${{ needs.validated-candidate.outputs.repository }}",
                "ref": "${{ needs.validated-candidate.outputs.revision }}",
                "persist-credentials": "false",
            },
            f"{filename}.{job_name} checkout",
        )

    if filename == "ci.yml":
        enforce_trusted_macos_release(
            job_entries["trusted-macos-release"], checkout_reference
        )

    final = job_entries["report-final"]
    require_scalar(
        final, "runs-on", "ubuntu-24.04", f"{filename}.report-final"
    )
    require_needs(
        final,
        ("validated-candidate", "report-pending", *candidate_jobs),
        f"{filename}.report-final",
    )
    require_scalar(
        final,
        "if",
        "${{ always() && github.event_name == 'workflow_run' && needs.validated-candidate.outputs.authorized == 'true' }}",
        f"{filename}.report-final",
    )
    require_permissions(
        required_entry(final, "permissions", f"{filename}.report-final"),
        {"statuses": "write"},
        f"{filename}.report-final.permissions",
    )
    final_steps, _ = workflow_steps(final, f"{filename}.report-final")
    if len(final_steps) != 1 or "uses" in final_steps[0]:
        raise ValueError(f"{filename} final reporter is not isolated")
    final_environment = {
        "GH_TOKEN": "${{ github.token }}",
        "CANDIDATE_SHA": "${{ needs.validated-candidate.outputs.revision }}",
        "STATUS_CONTEXT": build["status_context"],
        "SELECTED": "${{ needs.validated-candidate.outputs.selected }}",
        "PENDING_RESULT": "${{ needs.report-pending.result }}",
    }
    for job_name in candidate_jobs:
        variable = job_name.upper().replace("-", "_") + "_RESULT"
        final_environment[variable] = "${{ needs." + job_name + ".result }}"
    require_environment(
        final_steps[0], final_environment, f"{filename} final reporter"
    )
    final_script = require_run_fragments(
        final_steps[0],
        (
            "Candidate tests are not applicable",
            "state=failure",
            "state=success",
            "/statuses/${CANDIDATE_SHA}",
            '--raw-field context="$STATUS_CONTEXT"',
        ),
        f"{filename} final reporter",
    )
    require_run_sha256(
        final_steps[0],
        build["trusted_run_sha256"]["report-final"],
        f"{filename} final reporter",
    )
    for job_name in candidate_jobs:
        expected = job_name.upper().replace("-", "_") + "_RESULT"
        if f'"${expected}" = success' not in final_script:
            raise ValueError(
                f"{filename} final reporter ignores {job_name} result"
            )


def enforce_repository_policy(
    documents, contract, allowlist, repository_root
):
    policy = contract["policy_workflow"]
    builds = contract["build_workflows"]
    required = {policy["file"], *builds}
    missing = sorted(required - set(documents))
    if missing:
        raise ValueError(
            "required workflow is missing: " + ", ".join(missing)
        )
    unexpected = sorted(set(documents) - required)
    if unexpected:
        raise ValueError(
            "uncontracted workflow is present: " + ", ".join(unexpected)
        )
    checkout_revision = allowlist.get("actions/checkout")
    if checkout_revision is None:
        raise ValueError("actions/checkout is missing from the trusted allowlist")
    checkout_reference = f"actions/checkout@{checkout_revision}"
    enforce_policy_workflow(
        documents[policy["file"]], policy, checkout_reference,
        contract["protected_files"],
    )
    for filename, build in builds.items():
        enforce_build_workflow(
            documents[filename], filename, build, checkout_reference
        )
    enforce_protected_files(repository_root, contract["protected_files"])


def check(
    path, allowlist_path, enforce_policy=False, contract_path=None,
    repository_root=None,
):
    allowlist = load_allowlist(allowlist_path)
    references = 0
    violations = []
    documents = {}
    for workflow in workflow_files(path):
        if workflow.is_symlink() or not workflow.is_file():
            raise ValueError(f"unsafe workflow file: {workflow}")
        if workflow.stat().st_size > MAX_WORKFLOW_BYTES:
            raise ValueError(f"workflow file is too large: {workflow}")
        action_references, container_images, document = extract_uses(
            workflow.read_text(encoding="utf-8"), workflow
        )
        if path.is_file():
            relative = workflow.name
        else:
            relative = workflow.relative_to(path).as_posix()
        documents[relative] = document
        for line_number, reference in action_references:
            references += 1
            try:
                validate_reference(reference, allowlist)
            except ValueError as error:
                violations.append(
                    f"{workflow}:{line_number}: {reference}: {error}"
                )
        for line_number, image in container_images:
            try:
                validate_container_image(image)
            except ValueError as error:
                violations.append(
                    f"{workflow}:{line_number}: {image}: {error}"
                )
    if references == 0:
        raise ValueError("no action references were found")
    if violations:
        raise ValueError(
            "unapproved GitHub Actions references:\n" + "\n".join(violations)
        )
    if enforce_policy:
        try:
            if repository_root is None:
                if path.name != "workflows" or path.parent.name != ".github":
                    raise ValueError(
                        "candidate repository root must be specified"
                    )
                repository_root = path.parent.parent
            contract = load_contract(contract_path)
            enforce_repository_policy(
                documents, contract, allowlist, repository_root
            )
        except (json.JSONDecodeError, ValueError) as error:
            raise ValueError(f"workflow policy contract: {error}") from error


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--workflows", type=Path, required=True)
    parser.add_argument(
        "--allowlist",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "actions.lock",
    )
    parser.add_argument("--enforce-policy", action="store_true")
    parser.add_argument("--repository-root", type=Path)
    parser.add_argument(
        "--contract",
        type=Path,
        default=(
            Path(__file__).resolve().parents[1]
            / "workflow-policy-contract.json"
        ),
    )
    arguments = parser.parse_args()
    try:
        check(
            arguments.workflows,
            arguments.allowlist,
            arguments.enforce_policy,
            arguments.contract,
            arguments.repository_root,
        )
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1) from None


if __name__ == "__main__":
    main()
