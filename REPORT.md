# PES-VCS Lab Report

## Verification Checklist

Use these commands from the repository root to capture the required screenshots.

### Phase 1

```bash
make test_objects
./test_objects
find .pes/objects -type f
```

### Phase 2

```bash
make test_tree
./test_tree
find .pes/objects -type f
xxd .pes/objects/XX/YYYY... | head -20
```

### Phase 3

```bash
make pes
./pes init
echo "hello" > file1.txt
echo "world" > file2.txt
./pes add file1.txt file2.txt
./pes status
cat .pes/index
```

### Phase 4

```bash
./pes init
echo "Hello" > hello.txt
./pes add hello.txt
./pes commit -m "Initial commit"
echo "World" >> hello.txt
./pes add hello.txt
./pes commit -m "Add world"
echo "Goodbye" > bye.txt
./pes add bye.txt
./pes commit -m "Add farewell"
./pes log
find .pes -type f | sort
cat .pes/refs/heads/main
cat .pes/HEAD
```

### Final Integration Test

```bash
make test-integration
```

## Phase 5: Branching and Checkout

### Q5.1

To implement `pes checkout <branch>`, PES would first read `.pes/refs/heads/<branch>` to find the target commit. Then it would update `.pes/HEAD` to contain `ref: refs/heads/<branch>`. The working directory must also be changed to match the tree pointed to by that commit: parse the commit object, load its root tree, recursively load subtrees, write each blob to its path, create needed directories, and remove tracked files that are not present in the target tree.

The hard part is not changing `.pes/HEAD`; that is just writing a small file. The hard part is making the working directory transition safely. Checkout must avoid overwriting uncommitted work, must remove files that are tracked in the old commit but absent in the new one, must create directories in the right order, and should leave unrelated untracked files alone.

### Q5.2

A dirty working directory conflict can be detected by comparing three states: the current index, the working directory, and the target branch tree. For every tracked path in the index, stat the working file and compare its size and modification time with the index metadata. If the metadata differs, rehash the working file and compare the blob hash with the index hash. If the working hash differs, the file has uncommitted local changes.

Then parse the target branch tree and find the target blob hash for the same path. If the file is locally modified and the target branch wants a different blob hash, checkout must refuse because it would overwrite local work. If the target has the same blob hash, checkout can keep the file. If the target deletes the file but the working copy is locally modified, checkout must also refuse.

### Q5.3

Detached HEAD means `.pes/HEAD` stores a commit hash directly instead of `ref: refs/heads/main`. If a user commits in this state, the new commit's parent is the detached commit, and `.pes/HEAD` moves to the new commit hash. No branch name is updated, so the commits are reachable only through the detached HEAD value.

The user can recover those commits by creating or updating a branch file to point to the detached commit hash, for example by writing the hash into `.pes/refs/heads/recovered`. After that, changing `.pes/HEAD` to `ref: refs/heads/recovered` makes the commits reachable through a normal branch again.

## Phase 6: Garbage Collection and Space Reclamation

### Q6.1

Garbage collection can use mark-and-sweep. First, read every branch hash from `.pes/refs/heads/` and push those commits into a work queue. Maintain a hash set of reachable object IDs. While the queue is not empty, pop an object ID. If it was already marked, skip it. Otherwise, read the object, mark it reachable, and enqueue the objects it references. Commits reference their parent commit and root tree. Trees reference blobs and subtrees. Blobs reference nothing.

After marking, scan every object file under `.pes/objects/`. Convert each path back into its full hash. If the hash is not in the reachable set, delete that object file.

For a repository with 100,000 commits and 50 branches, the number of commits visited is at most 100,000 because shared history is marked once. The total object visits are 100,000 commit objects plus every reachable tree and blob object referenced by those commits. The exact total depends on project size and how much content changes, but the algorithm is linear in the number of reachable objects, not branches times commits.

### Q6.2

Garbage collection is dangerous during a concurrent commit because object creation and reference updates are not one atomic operation. A commit writes blob and tree objects first, then writes the commit object, then updates the branch ref. During the window before the branch ref is updated, a GC process scanning refs cannot see the new commit. It may classify the newly written tree or blob objects as unreachable and delete them. The commit may then finish and point to objects that no longer exist.

Git avoids this with conservative rules: it uses locks for ref updates, treats very recent unreachable objects as protected, writes objects before publishing refs, and avoids pruning objects that might have been created by concurrent operations. Real Git also uses temporary files, lock files, reflogs, and grace periods so that objects are not immediately removed just because a single reachability scan does not see them.
