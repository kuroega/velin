// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
import SwiftUI
import VelinCore

// Browses one AList server. Shows the sign-in form until signed in, then the
// folder (the last one browsed, unless a link asked for another).
struct AListView: View {
    @EnvironmentObject private var model: PlayerModel
    @EnvironmentObject private var downloads: DownloadManager
    let initialPath: String?
    @State private var signedIn = false
    @State private var path = "/"
    @State private var folder: VLNAListFolder?
    @State private var loading = false
    @State private var problem: String?
    @State private var passwordFolder: String?   // Asking for this folder's password.
    @State private var folderPassword = ""
    @State private var showingSignOut = false

    var body: some View {
        Group {
            if signedIn { browser } else { AListSignIn { signedIn = true; load(initialPath ?? model.alist.lastFolder) } }
        }
        .navigationTitle(signedIn ? title : "AList")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear {
            signedIn = model.alist.isSignedIn
            if signedIn && folder == nil { load(initialPath ?? model.alist.lastFolder) }
        }
        .alert("Folder password", isPresented: Binding(get: { passwordFolder != nil }, set: { if !$0 { passwordFolder = nil } })) {
            SecureField("Password", text: $folderPassword)
            Button("Open") {
                if let target = passwordFolder {
                    model.alist.setPassword(folderPassword, forFolder: target)
                    folderPassword = ""
                    load(target)
                }
            }
            Button("Cancel", role: .cancel) { folderPassword = "" }
        } message: { Text(passwordFolder ?? "") }
        .confirmationDialog("Sign out of \(model.alist.server)?", isPresented: $showingSignOut, titleVisibility: .visible) {
            Button("Sign Out", role: .destructive) { model.alist.signOut(); folder = nil; signedIn = false }
        }
    }

    private var title: String { path == "/" ? "AList" : (path as NSString).lastPathComponent }

    private var browser: some View {
        List {
            if let problem {
                Section { Label(problem, systemImage: "exclamationmark.triangle").foregroundStyle(.red) }
            }
            if path != "/" {
                Button { load(parent(of: path)) } label: {
                    Label("Up to \(parent(of: path) == "/" ? "AList" : (parent(of: path) as NSString).lastPathComponent)", systemImage: "arrow.turn.left.up")
                }
            }
            if let folder {
                let visible = folder.entries.filter { $0.isDirectory || $0.isPlayable }
                if visible.isEmpty && !loading { Text("Nothing playable here").foregroundStyle(.secondary) }
                ForEach(visible, id: \.path) { entry in
                    let key = VLNAListLink(entry.path)
                    Button { open(entry, in: folder) } label: {
                        AListRow(entry: entry, history: model.library.entry(forKey: key),
                                 download: entry.isDirectory ? nil : downloadState(key))
                    }
                    .buttonStyle(.plain)
                    .swipeActions(edge: .leading) {
                        if !entry.isDirectory && downloadState(key) == nil {
                            Button { problem = model.downloadAList(entry, in: folder) } label: { Label("Download", systemImage: "arrow.down.circle") }
                                .tint(.accentColor)
                        }
                    }
                    .contextMenu {
                        if !entry.isDirectory {
                            Button("Play", systemImage: "play") { open(entry, in: folder) }
                            if let file = downloads.downloadedFile(for: key) {
                                Button("Play Downloaded Copy", systemImage: "iphone") { model.openDocument(file) }
                            } else if downloads.job(for: key) == nil {
                                Button("Download", systemImage: "arrow.down.circle") { problem = model.downloadAList(entry, in: folder) }
                            }
                        }
                    }
                }
            }
        }
        .overlay { if loading && folder == nil { ProgressView() } }
        .refreshable { await reload() }
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Menu {
                    Text("\(model.alist.username) · \(model.alist.server)")
                    Button("Refresh", systemImage: "arrow.clockwise") { load(path, refresh: true) }
                    if let folder, folder.entries.contains(where: { $0.isPlayable && !$0.isDirectory }) {
                        Button("Download All in Folder", systemImage: "arrow.down.circle") { problem = model.downloadAList(folder: folder) }
                    }
                    Button("Sign Out", systemImage: "rectangle.portrait.and.arrow.right", role: .destructive) { showingSignOut = true }
                } label: { Image(systemName: loading ? "ellipsis" : "person.crop.circle") }
            }
        }
    }

    private func downloadState(_ key: String) -> AListRow.Download? {
        if downloads.downloadedFile(for: key) != nil { return .done }
        guard let job = downloads.job(for: key) else { return nil }
        if case .failed = job.state { return .failed }
        return .running(job.fraction ?? 0)
    }

    private func open(_ entry: VLNAListEntry, in folder: VLNAListFolder) {
        if entry.isDirectory { load(entry.path) } else { model.openAList(folder: folder, path: entry.path) }
    }

    private func load(_ target: String, refresh: Bool = false) {
        loading = true
        model.alist.listFolder(target, refresh: refresh) { result, error in
            loading = false
            handle(target: target, result: result, error: error)
        }
    }

    private func reload() async {
        await withCheckedContinuation { continuation in
            model.alist.listFolder(path, refresh: true) { result, error in
                handle(target: path, result: result, error: error)
                continuation.resume()
            }
        }
    }

    private func handle(target: String, result: VLNAListFolder?, error: Error?) {
        if let result {
            folder = result
            path = result.path
            problem = nil
            return
        }
        guard let error else { return }
        switch (error as? VLNAListError)?.code {
        case .unauthorized:
            signedIn = false
        case .forbidden:
            passwordFolder = target
        default:
            // An unreachable folder from history falls back to the root.
            if folder == nil && target != "/" { load("/") }
            problem = error.localizedDescription
        }
    }

    private func parent(of path: String) -> String {
        let parent = (path as NSString).deletingLastPathComponent
        return parent.isEmpty ? "/" : parent
    }
}

private struct AListRow: View {
    let entry: VLNAListEntry
    let history: VLNHistoryEntry?
    var download: Download? = nil

    enum Download: Equatable { case running(Double), done, failed }

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: entry.isDirectory ? "folder.fill" : "film")
                .foregroundStyle(entry.isDirectory ? Color.accentColor : .secondary).frame(width: 26)
            VStack(alignment: .leading, spacing: 3) {
                Text(entry.name).foregroundStyle(.primary).lineLimit(2)
                if !entry.isDirectory {
                    Text(detail).font(.caption).foregroundStyle(.secondary)
                    if let history, history.duration > 0, history.position > 5 {
                        ProgressView(value: min(history.position / history.duration, 1))
                            .tint(isFinished(position: history.position, duration: history.duration) ? .green : .accentColor)
                    }
                }
            }
            Spacer()
            switch download {
            case .running(let fraction):
                ProgressView(value: fraction).progressViewStyle(.circular).controlSize(.small)
                    .overlay { Image(systemName: "arrow.down").font(.system(size: 8, weight: .bold)).foregroundStyle(.tint) }
            case .done: Image(systemName: "arrow.down.circle.fill").foregroundStyle(.green)
            case .failed: Image(systemName: "exclamationmark.circle").foregroundStyle(.orange)
            case nil: EmptyView()
            }
            if entry.isDirectory { Image(systemName: "chevron.right").font(.caption).foregroundStyle(.tertiary) }
        }
    }

    private var detail: String {
        var parts = [ByteCountFormatter.string(fromByteCount: entry.size, countStyle: .file)]
        if entry.modified.count >= 10 { parts.append(String(entry.modified.prefix(10))) }
        return parts.joined(separator: " · ")
    }
}

struct AListSignIn: View {
    @EnvironmentObject private var model: PlayerModel
    var onSignedIn: () -> Void
    @State private var server = ""
    @State private var username = ""
    @State private var password = ""
    @State private var otpCode = ""
    @State private var needsCode = false
    @State private var busy = false
    @State private var problem: String?

    var body: some View {
        Form {
            Section {
                TextField("Server, e.g. 192.168.1.10:5244", text: $server)
                    .textInputAutocapitalization(.never).autocorrectionDisabled().keyboardType(.URL)
                TextField("Username", text: $username).textInputAutocapitalization(.never).autocorrectionDisabled()
                SecureField("Password", text: $password)
                if needsCode {
                    TextField("Two-factor code", text: $otpCode).keyboardType(.numberPad)
                }
            } footer: {
                Text("Velin signs in with AList's hashed password endpoint and keeps the session encrypted on this iPhone.")
            }
            if let problem { Section { Text(problem).foregroundStyle(.red) } }
            Section {
                Button {
                    signIn()
                } label: {
                    HStack { Text("Sign In"); if busy { Spacer(); ProgressView() } }
                }
                .disabled(busy || server.isEmpty || username.isEmpty)
            }
        }
        .onAppear {
            if server.isEmpty { server = model.alist.server }
            if username.isEmpty { username = model.alist.username }
        }
    }

    private func signIn() {
        busy = true
        problem = nil
        model.alist.signIn(toServer: server, username: username, password: password, otpCode: needsCode ? otpCode : nil) { error in
            busy = false
            guard let error else { password = ""; onSignedIn(); return }
            if (error as? VLNAListError)?.code == .otpRequired {
                needsCode = true
                problem = "Enter the code from your authenticator app."
            } else {
                problem = error.localizedDescription
            }
        }
    }
}
