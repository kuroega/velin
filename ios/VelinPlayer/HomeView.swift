// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
import SwiftUI
import UniformTypeIdentifiers
import VelinCore

struct HomeView: View {
    @EnvironmentObject private var model: PlayerModel
    @EnvironmentObject private var downloads: DownloadManager
    @Binding var path: NavigationPath
    @State private var importing = false
    @State private var documents: [URL] = []
    @State private var showingURLPrompt = false
    @State private var urlText = ""
    @State private var problem: String?
    @State private var deleting: URL?

    var body: some View {
        List {
            if !model.recents.isEmpty {
                Section("Continue Watching") {
                    ForEach(model.recents.prefix(30), id: \.key) { entry in
                        Button { reopen(entry) } label: { HistoryRow(entry: entry) }
                            .buttonStyle(.plain)
                            .swipeActions {
                                Button(role: .destructive) { model.forget(entry) } label: { Label("Remove", systemImage: "trash") }
                            }
                    }
                }
            }
            Section("Sources") {
                NavigationLink(value: Route.alist(nil)) {
                    Label {
                        VStack(alignment: .leading) {
                            Text("AList")
                            Text(model.alist.isSignedIn ? "\(model.alist.username) · \(model.alist.server)" : "Not signed in")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                    } icon: { Image(systemName: "server.rack") }
                }
                Button { importing = true } label: { Label("Open from Files…", systemImage: "folder") }
                Button { showingURLPrompt = true } label: { Label("Open URL…", systemImage: "link") }
            }
            if !downloads.jobs.isEmpty {
                Section("Downloading") {
                    ForEach(downloads.jobs) { job in
                        DownloadRow(job: job)
                            .swipeActions {
                                if case .failed = job.state {
                                    Button("Remove", role: .destructive) { downloads.dismiss(job) }
                                    Button("Retry") { downloads.retry(job) }.tint(.accentColor)
                                } else {
                                    Button("Cancel", role: .destructive) { downloads.cancel(job) }
                                }
                            }
                            .contextMenu {
                                if case .failed = job.state {
                                    Button("Retry", systemImage: "arrow.clockwise") { downloads.retry(job) }
                                    Button("Remove", systemImage: "xmark", role: .destructive) { downloads.dismiss(job) }
                                } else {
                                    Button("Cancel Download", systemImage: "xmark", role: .destructive) { downloads.cancel(job) }
                                }
                            }
                    }
                }
            }
            Section {
                if documents.isEmpty {
                    Text("Copy videos into Velin with the Files app (On My iPhone › Velin) or Finder.")
                        .font(.callout).foregroundStyle(.secondary)
                }
                ForEach(documents, id: \.self) { url in
                    Button { model.openDocument(url) } label: {
                        Label { Text(url.lastPathComponent) } icon: { Image(systemName: "film").foregroundStyle(.tint) }
                    }
                    .buttonStyle(.plain)
                    .swipeActions {
                        Button(role: .destructive) { deleting = url } label: { Label("Delete", systemImage: "trash") }
                    }
                }
            } header: { Text("On This iPhone") } footer: {
                if let free = DownloadManager.freeSpace { Text("\(DownloadManager.bytes(free)) available") }
            }
        }
        .navigationTitle("Velin")
        .toolbar {
            NavigationLink(value: Route.settings) { Image(systemName: "gearshape") }
        }
        .refreshable { refresh() }
        .onAppear(perform: refresh)
        .onChange(of: model.showPlayer) { _, showing in if !showing { refresh() } }
        .onChange(of: downloads.finishedCount) { refresh() }
        .confirmationDialog("Delete \(deleting?.lastPathComponent ?? "")?", isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } }),
                            titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                if let url = deleting { downloads.deleteFile(url); refresh() }
            }
        } message: { Text("This removes the file from this iPhone.") }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.movie, .audio, .data]) { result in
            if case .success(let url) = result { model.openImported(url) }
        }
        .alert("Open URL", isPresented: $showingURLPrompt) {
            TextField("https://…", text: $urlText).textInputAutocapitalization(.never).keyboardType(.URL)
            Button("Play") {
                if let url = enteredURL { model.openRemote(url) } else { problem = "Enter an http or https link." }
            }
            Button("Download") {
                if let url = enteredURL { problem = model.downloadRemote(url) } else { problem = "Enter an http or https link." }
            }
            Button("Cancel", role: .cancel) {}
        }
        .alert("Velin", isPresented: Binding(get: { problem != nil }, set: { if !$0 { problem = nil } })) {
            Button("OK", role: .cancel) {}
        } message: { Text(problem ?? "") }
    }

    private var enteredURL: URL? {
        guard let url = URL(string: urlText.trimmingCharacters(in: .whitespaces)),
              ["http", "https"].contains(url.scheme?.lowercased() ?? "") else { return nil }
        return url
    }

    private func refresh() {
        documents = MediaSources.documentFiles()
        model.refreshRecents()
    }

    private func reopen(_ entry: VLNHistoryEntry) {
        let found = model.openHistory(entry) { error in
            guard let error else { return }
            if (error as? VLNAListError)?.code == .unauthorized {
                path.append(Route.alist(nil))
            } else { problem = error.localizedDescription }
        }
        if !found { problem = "\(entry.title) is no longer available." }
    }
}

// Watched: within the last 10 seconds, or the last 5% of a short file.
func isFinished(position: Double, duration: Double) -> Bool {
    duration > 0 && position >= max(duration - 10, duration * 0.95)
}

enum Route: Hashable {
    case alist(String?)  // Folder to open; nil for the last one.
    case settings
}

private struct HistoryRow: View {
    let entry: VLNHistoryEntry

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: icon).font(.title3).foregroundStyle(.tint).frame(width: 28)
            VStack(alignment: .leading, spacing: 4) {
                Text(entry.title).foregroundStyle(.primary).lineLimit(2)
                if entry.duration > 0 {
                    ProgressView(value: min(entry.position / entry.duration, 1)).tint(finished ? .green : .accentColor)
                }
                Text(caption).font(.caption).foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 2)
    }

    private var finished: Bool { isFinished(position: entry.position, duration: entry.duration) }

    private var icon: String {
        if entry.key.hasPrefix("velin://alist") { return "server.rack" }
        if entry.key.hasPrefix("http") { return "globe" }
        return "film"
    }

    private var caption: String {
        let when = entry.lastPlayed.formatted(.relative(presentation: .named))
        if finished { return "Watched · \(when)" }
        if entry.duration > 0 { return "\(timeText(entry.position)) of \(timeText(entry.duration)) · \(when)" }
        return when
    }
}

private struct DownloadRow: View {
    let job: DownloadManager.Job

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: failed ? "exclamationmark.triangle.fill" : "arrow.down.circle")
                .font(.title3).foregroundStyle(failed ? Color.orange : Color.accentColor).frame(width: 28)
            VStack(alignment: .leading, spacing: 4) {
                Text(job.name).lineLimit(2)
                if !failed {
                    if let fraction = job.fraction { ProgressView(value: fraction) } else { ProgressView(value: 0).opacity(job.received > 0 ? 1 : 0.4) }
                }
                Text(caption).font(.caption).foregroundStyle(failed ? .orange : .secondary).lineLimit(2)
            }
        }
        .padding(.vertical, 2)
    }

    private var failed: Bool { if case .failed = job.state { return true }; return false }

    private var caption: String {
        if case .failed(let message) = job.state { return "Failed: \(message) Swipe to retry." }
        if job.received == 0 { return job.state == .waiting ? "Waiting…" : "Starting…" }
        let received = DownloadManager.bytes(job.received)
        guard let fraction = job.fraction else { return received }
        return "\(received) of \(DownloadManager.bytes(job.expected)) · \(Int(fraction * 100))%"
    }
}
