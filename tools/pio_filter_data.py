from pathlib import Path
import shutil

Import("env")
from SCons.Script import COMMAND_LINE_TARGETS


def _prepare_filtered_data_dir(source, target, env):
    project_dir = Path(env.subst("$PROJECT_DIR"))
    data_dir = Path(env.subst("$PROJECT_DATA_DIR"))
    filtered_dir = project_dir / ".pio" / "data_filtered"

    if filtered_dir.exists():
        shutil.rmtree(filtered_dir)

    def _ignore(path, names):
        if Path(path).resolve() == data_dir.resolve():
            return {"dsl_available"} if "dsl_available" in names else set()
        return set()

    shutil.copytree(data_dir, filtered_dir, ignore=_ignore)
    env.Replace(PROJECT_DATA_DIR=str(filtered_dir))


if "buildfs" in COMMAND_LINE_TARGETS or "uploadfs" in COMMAND_LINE_TARGETS:
    _prepare_filtered_data_dir(None, None, env)
